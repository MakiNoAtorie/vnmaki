#include "engine/render/sdl_gpu_renderer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "engine/platform/sdl_platform.hpp"
#include "engine/render/builtin_shaders.hpp"
#include "engine/render/frame_recorder.hpp"

namespace vnmaki {

namespace {

using render_detail::Batch;
using render_detail::FrameRecorder;
using render_detail::kIndicesPerQuad;
using render_detail::kQuadIndices;
using render_detail::kVerticesPerQuad;
using render_detail::RecordedPass;
using render_detail::SpriteVertex;

/// The most quads one draw call covers: 16-bit indices reach 65536 vertices. A longer batch is
/// split into several draw calls.
constexpr std::uint32_t kMaxQuadsPerDraw = 65536 / kVerticesPerQuad;
constexpr SDL_GPUTextureFormat kTextureFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
constexpr std::uint32_t kBytesPerPixel = 4;
/// Vertex buffers grow to at least this many bytes.
constexpr std::uint32_t kMinVertexBufferBytes = 64 * 1024;

[[noreturn]] void throw_sdl_error(const std::string& what) {
  throw RenderError{what + ": " + SDL_GetError()};
}

/// Releases GPU objects of any kind through the device that made them.
struct GpuReleaser {
  SDL_GPUDevice* device = nullptr;

  void operator()(SDL_GPUTexture* texture) const { SDL_ReleaseGPUTexture(device, texture); }
  void operator()(SDL_GPUBuffer* buffer) const { SDL_ReleaseGPUBuffer(device, buffer); }
  void operator()(SDL_GPUTransferBuffer* buffer) const {
    SDL_ReleaseGPUTransferBuffer(device, buffer);
  }
  void operator()(SDL_GPUSampler* sampler) const { SDL_ReleaseGPUSampler(device, sampler); }
  void operator()(SDL_GPUShader* shader) const { SDL_ReleaseGPUShader(device, shader); }
  void operator()(SDL_GPUGraphicsPipeline* pipeline) const {
    SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
  }
};

template <typename T>
using GpuPtr = std::unique_ptr<T, GpuReleaser>;

struct DeviceDestroyer {
  void operator()(SDL_GPUDevice* device) const { SDL_DestroyGPUDevice(device); }
};

/// Keeps SDL's video subsystem running (the Vulkan driver needs it).
class VideoSubsystem {
 public:
  VideoSubsystem() {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
      throw_sdl_error("cannot start SDL video support");
    }
  }
  ~VideoSubsystem() {
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    // Release SDL's remaining global state once nothing else (the platform, audio) uses SDL.
    if (SDL_WasInit(0) == 0) {
      SDL_Quit();
    }
  }
  VideoSubsystem(const VideoSubsystem&) = delete;
  VideoSubsystem& operator=(const VideoSubsystem&) = delete;
  VideoSubsystem(VideoSubsystem&&) = delete;
  VideoSubsystem& operator=(VideoSubsystem&&) = delete;
};

/// The device's claim on a window's swapchain.
class WindowClaim {
 public:
  WindowClaim(SDL_GPUDevice* device, SDL_Window* window) : device_(device), window_(window) {
    if (!SDL_ClaimWindowForGPUDevice(device_, window_)) {
      throw_sdl_error("cannot attach the GPU device to the window");
    }
  }
  ~WindowClaim() { SDL_ReleaseWindowFromGPUDevice(device_, window_); }
  WindowClaim(const WindowClaim&) = delete;
  WindowClaim& operator=(const WindowClaim&) = delete;
  WindowClaim(WindowClaim&&) = delete;
  WindowClaim& operator=(WindowClaim&&) = delete;

  [[nodiscard]] SDL_Window* window() const { return window_; }

 private:
  SDL_GPUDevice* device_;
  SDL_Window* window_;
};

/// The shader formats the build embedded (VNMAKI_SHADER_FORMATS).
SDL_GPUShaderFormat embedded_formats() {
  const ShaderBytecode vertex = shaders::sprite_vert();
  SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_INVALID;
  if (!vertex.spirv.empty()) {
    formats |= SDL_GPU_SHADERFORMAT_SPIRV;
  }
  if (!vertex.msl.empty()) {
    formats |= SDL_GPU_SHADERFORMAT_MSL;
  }
  if (!vertex.dxil.empty()) {
    formats |= SDL_GPU_SHADERFORMAT_DXIL;
  }
  return formats;
}

std::span<const std::uint8_t> code_for(const ShaderBytecode& bytecode, SDL_GPUShaderFormat format) {
  switch (format) {
    case SDL_GPU_SHADERFORMAT_MSL:
      return bytecode.msl;
    case SDL_GPU_SHADERFORMAT_DXIL:
      return bytecode.dxil;
    default:
      return bytecode.spirv;
  }
}

SDL_FColor premultiplied(const Color& color) {
  return SDL_FColor{
      .r = color.r * color.a, .g = color.g * color.a, .b = color.b * color.a, .a = color.a};
}

SDL_GPUColorTargetBlendState blend_state(BlendMode mode) {
  SDL_GPUColorTargetBlendState state{};
  switch (mode) {
    case BlendMode::kAlpha:
      state.enable_blend = true;
      state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
      state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
      state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
      state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
      break;
    case BlendMode::kAdditive:
      state.enable_blend = true;
      state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
      state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
      state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
      state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
      break;
    case BlendMode::kReplace:
      state.enable_blend = false;
      break;
  }
  state.color_blend_op = SDL_GPU_BLENDOP_ADD;
  state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
  return state;
}

}  // namespace

struct SdlGpuRenderer::Impl {
  struct Texture {
    GpuPtr<SDL_GPUTexture> texture;
    Size size{};
    TextureFilter filter = TextureFilter::kLinear;
    bool render_target = false;
  };
  struct Shader {
    GpuPtr<SDL_GPUShader> shader;
    bool takes_uniforms = false;
  };
  using PipelineKey = std::tuple<std::uint32_t, BlendMode, SDL_GPUTextureFormat>;

  // Declaration order is destruction order reversed: SDL goes last, the device next to last.
  std::unique_ptr<VideoSubsystem> video;
  std::unique_ptr<SDL_GPUDevice, DeviceDestroyer> device;
  std::unique_ptr<WindowClaim> window;
  SDL_GPUShaderFormat shader_format = SDL_GPU_SHADERFORMAT_INVALID;

  GpuPtr<SDL_GPUShader> sprite_vertex;
  GpuPtr<SDL_GPUShader> sprite_fragment;
  GpuPtr<SDL_GPUSampler> linear_sampler;
  GpuPtr<SDL_GPUSampler> nearest_sampler;
  GpuPtr<SDL_GPUBuffer> index_buffer;
  GpuPtr<SDL_GPUBuffer> vertex_buffer;
  std::uint32_t vertex_buffer_bytes = 0;
  GpuPtr<SDL_GPUTransferBuffer> vertex_upload;
  std::uint32_t vertex_upload_bytes = 0;

  std::unordered_map<std::uint32_t, Texture> textures;
  std::uint32_t next_texture = 1;
  std::unordered_map<std::uint32_t, Shader> custom_shaders;
  std::uint32_t next_shader = 1;
  std::map<PipelineKey, GpuPtr<SDL_GPUGraphicsPipeline>> pipelines;

  bool in_frame = false;
  FrameRecorder recorder;
  std::vector<TextureId> textures_to_destroy;
  std::vector<ShaderId> shaders_to_destroy;

  explicit Impl(const RendererConfig& config, std::unique_ptr<VideoSubsystem> video_subsystem,
                SDL_Window* sdl_window);
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;
  ~Impl();

  [[nodiscard]] GpuReleaser releaser() const { return GpuReleaser{device.get()}; }
  [[nodiscard]] GpuPtr<SDL_GPUShader> make_shader(const ShaderBytecode& bytecode,
                                                  SDL_GPUShaderStage stage) const;
  [[nodiscard]] GpuPtr<SDL_GPUSampler> make_sampler(SDL_GPUFilter filter) const;
  [[nodiscard]] GpuPtr<SDL_GPUBuffer> make_buffer(SDL_GPUBufferUsageFlags usage,
                                                  std::uint32_t bytes) const;
  [[nodiscard]] GpuPtr<SDL_GPUTransferBuffer> make_transfer_buffer(SDL_GPUTransferBufferUsage usage,
                                                                   std::uint32_t bytes) const;
  void upload_index_buffer();
  [[nodiscard]] const Texture& texture(TextureId id) const;
  [[nodiscard]] TextureId add_texture(Size size, TextureFilter filter, bool render_target);
  [[nodiscard]] SDL_GPUGraphicsPipeline* pipeline(ShaderId shader, BlendMode blend,
                                                  SDL_GPUTextureFormat format);
  void upload_vertices(SDL_GPUCommandBuffer* commands);
  /// Records the frame's passes into `commands`; returns the number of draw calls.
  std::uint32_t record_passes(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* swapchain,
                              Size swapchain_size);
  void release_destroyed();
};

SdlGpuRenderer::Impl::Impl(const RendererConfig& config,
                           std::unique_ptr<VideoSubsystem> video_subsystem, SDL_Window* sdl_window)
    : video(std::move(video_subsystem)) {
  const SDL_GPUShaderFormat available = embedded_formats();
  device.reset(SDL_CreateGPUDevice(available, config.debug, nullptr));
  if (!device) {
    throw_sdl_error("cannot create a GPU device");
  }
  const SDL_GPUShaderFormat supported = SDL_GetGPUShaderFormats(device.get()) & available;
  for (const SDL_GPUShaderFormat format :
       {SDL_GPU_SHADERFORMAT_MSL, SDL_GPU_SHADERFORMAT_DXIL, SDL_GPU_SHADERFORMAT_SPIRV}) {
    if ((supported & format) != 0) {
      shader_format = format;
      break;
    }
  }
  if (shader_format == SDL_GPU_SHADERFORMAT_INVALID) {
    throw RenderError{"the GPU driver takes none of the shader formats this build embeds"};
  }

  if (sdl_window != nullptr) {
    window = std::make_unique<WindowClaim>(device.get(), sdl_window);
    const SDL_GPUPresentMode present_mode =
        config.vsync ? SDL_GPU_PRESENTMODE_VSYNC
        : SDL_WindowSupportsGPUPresentMode(device.get(), sdl_window, SDL_GPU_PRESENTMODE_IMMEDIATE)
            ? SDL_GPU_PRESENTMODE_IMMEDIATE
            : SDL_GPU_PRESENTMODE_VSYNC;
    if (!SDL_SetGPUSwapchainParameters(device.get(), sdl_window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                       present_mode)) {
      throw_sdl_error("cannot configure the window's swapchain");
    }
  }

  sprite_vertex = make_shader(shaders::sprite_vert(), SDL_GPU_SHADERSTAGE_VERTEX);
  sprite_fragment = make_shader(shaders::sprite_frag(), SDL_GPU_SHADERSTAGE_FRAGMENT);
  linear_sampler = make_sampler(SDL_GPU_FILTER_LINEAR);
  nearest_sampler = make_sampler(SDL_GPU_FILTER_NEAREST);
  upload_index_buffer();
}

SdlGpuRenderer::Impl::~Impl() {
  // Let the GPU finish with everything before it is released.
  if (device) {
    SDL_WaitForGPUIdle(device.get());
  }
}

GpuPtr<SDL_GPUShader> SdlGpuRenderer::Impl::make_shader(const ShaderBytecode& bytecode,
                                                        SDL_GPUShaderStage stage) const {
  const std::span<const std::uint8_t> code = code_for(bytecode, shader_format);
  if (code.empty()) {
    throw RenderError{"the shader has no bytecode for this GPU driver"};
  }
  SDL_GPUShaderCreateInfo info{};
  info.code_size = code.size();
  info.code = code.data();
  info.entrypoint = shader_format == SDL_GPU_SHADERFORMAT_MSL ? "main0" : "main";
  info.format = shader_format;
  info.stage = stage;
  info.num_samplers = bytecode.samplers;
  info.num_uniform_buffers = bytecode.uniform_buffers;
  GpuPtr<SDL_GPUShader> shader{SDL_CreateGPUShader(device.get(), &info), releaser()};
  if (!shader) {
    throw_sdl_error("cannot create a shader");
  }
  return shader;
}

GpuPtr<SDL_GPUSampler> SdlGpuRenderer::Impl::make_sampler(SDL_GPUFilter filter) const {
  SDL_GPUSamplerCreateInfo info{};
  info.min_filter = filter;
  info.mag_filter = filter;
  info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  GpuPtr<SDL_GPUSampler> sampler{SDL_CreateGPUSampler(device.get(), &info), releaser()};
  if (!sampler) {
    throw_sdl_error("cannot create a texture sampler");
  }
  return sampler;
}

GpuPtr<SDL_GPUBuffer> SdlGpuRenderer::Impl::make_buffer(SDL_GPUBufferUsageFlags usage,
                                                        std::uint32_t bytes) const {
  SDL_GPUBufferCreateInfo info{};
  info.usage = usage;
  info.size = bytes;
  GpuPtr<SDL_GPUBuffer> buffer{SDL_CreateGPUBuffer(device.get(), &info), releaser()};
  if (!buffer) {
    throw_sdl_error("cannot create a GPU buffer");
  }
  return buffer;
}

GpuPtr<SDL_GPUTransferBuffer> SdlGpuRenderer::Impl::make_transfer_buffer(
    SDL_GPUTransferBufferUsage usage, std::uint32_t bytes) const {
  SDL_GPUTransferBufferCreateInfo info{};
  info.usage = usage;
  info.size = bytes;
  GpuPtr<SDL_GPUTransferBuffer> buffer{SDL_CreateGPUTransferBuffer(device.get(), &info),
                                       releaser()};
  if (!buffer) {
    throw_sdl_error("cannot create a GPU transfer buffer");
  }
  return buffer;
}

void SdlGpuRenderer::Impl::upload_index_buffer() {
  std::vector<std::uint16_t> indices;
  indices.reserve(std::size_t{kMaxQuadsPerDraw} * kIndicesPerQuad);
  for (std::uint32_t quad = 0; quad < kMaxQuadsPerDraw; ++quad) {
    for (const std::uint32_t corner : kQuadIndices) {
      indices.push_back(static_cast<std::uint16_t>((quad * kVerticesPerQuad) + corner));
    }
  }
  const auto bytes = static_cast<std::uint32_t>(indices.size() * sizeof(std::uint16_t));
  index_buffer = make_buffer(SDL_GPU_BUFFERUSAGE_INDEX, bytes);
  const GpuPtr<SDL_GPUTransferBuffer> upload =
      make_transfer_buffer(SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes);
  void* mapped = SDL_MapGPUTransferBuffer(device.get(), upload.get(), false);
  if (mapped == nullptr) {
    throw_sdl_error("cannot map a GPU transfer buffer");
  }
  std::memcpy(mapped, indices.data(), bytes);
  SDL_UnmapGPUTransferBuffer(device.get(), upload.get());

  SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device.get());
  if (commands == nullptr) {
    throw_sdl_error("cannot start GPU commands");
  }
  SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
  const SDL_GPUTransferBufferLocation source{.transfer_buffer = upload.get(), .offset = 0};
  const SDL_GPUBufferRegion destination{.buffer = index_buffer.get(), .offset = 0, .size = bytes};
  SDL_UploadToGPUBuffer(copy, &source, &destination, false);
  SDL_EndGPUCopyPass(copy);
  if (!SDL_SubmitGPUCommandBuffer(commands)) {
    throw_sdl_error("cannot submit GPU commands");
  }
}

const SdlGpuRenderer::Impl::Texture& SdlGpuRenderer::Impl::texture(TextureId id) const {
  const auto found = textures.find(id.value);
  if (found == textures.end()) {
    throw RenderError{"unknown texture id " + std::to_string(id.value)};
  }
  return found->second;
}

TextureId SdlGpuRenderer::Impl::add_texture(Size size, TextureFilter filter, bool render_target) {
  if (size.width <= 0 || size.height <= 0) {
    throw RenderError{"a texture needs a positive size, not " + std::to_string(size.width) + "x" +
                      std::to_string(size.height)};
  }
  SDL_GPUTextureCreateInfo info{};
  info.type = SDL_GPU_TEXTURETYPE_2D;
  info.format = kTextureFormat;
  info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
  if (render_target) {
    info.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
  }
  info.width = static_cast<std::uint32_t>(size.width);
  info.height = static_cast<std::uint32_t>(size.height);
  info.layer_count_or_depth = 1;
  info.num_levels = 1;
  info.sample_count = SDL_GPU_SAMPLECOUNT_1;
  GpuPtr<SDL_GPUTexture> gpu_texture{SDL_CreateGPUTexture(device.get(), &info), releaser()};
  if (!gpu_texture) {
    throw_sdl_error("cannot create a " + std::to_string(size.width) + "x" +
                    std::to_string(size.height) + " texture");
  }
  const TextureId id{next_texture++};
  textures.emplace(id.value, Texture{.texture = std::move(gpu_texture),
                                     .size = size,
                                     .filter = filter,
                                     .render_target = render_target});
  return id;
}

SDL_GPUGraphicsPipeline* SdlGpuRenderer::Impl::pipeline(ShaderId shader, BlendMode blend,
                                                        SDL_GPUTextureFormat format) {
  const PipelineKey key{shader.value, blend, format};
  if (const auto found = pipelines.find(key); found != pipelines.end()) {
    return found->second.get();
  }
  SDL_GPUShader* fragment = shader == kSpriteShader ? sprite_fragment.get()
                                                    : custom_shaders.at(shader.value).shader.get();

  const SDL_GPUVertexBufferDescription vertex_buffer_description{
      .slot = 0,
      .pitch = sizeof(SpriteVertex),
      .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
      .instance_step_rate = 0};
  const std::array<SDL_GPUVertexAttribute, 3> attributes{{
      {.location = 0,
       .buffer_slot = 0,
       .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
       .offset = offsetof(SpriteVertex, x)},
      {.location = 1,
       .buffer_slot = 0,
       .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
       .offset = offsetof(SpriteVertex, u)},
      {.location = 2,
       .buffer_slot = 0,
       .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
       .offset = offsetof(SpriteVertex, r)},
  }};
  SDL_GPUColorTargetDescription color_target{};
  color_target.format = format;
  color_target.blend_state = blend_state(blend);

  SDL_GPUGraphicsPipelineCreateInfo info{};
  info.vertex_shader = sprite_vertex.get();
  info.fragment_shader = fragment;
  info.vertex_input_state.vertex_buffer_descriptions = &vertex_buffer_description;
  info.vertex_input_state.num_vertex_buffers = 1;
  info.vertex_input_state.vertex_attributes = attributes.data();
  info.vertex_input_state.num_vertex_attributes = static_cast<std::uint32_t>(attributes.size());
  info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
  info.target_info.color_target_descriptions = &color_target;
  info.target_info.num_color_targets = 1;
  GpuPtr<SDL_GPUGraphicsPipeline> created{SDL_CreateGPUGraphicsPipeline(device.get(), &info),
                                          releaser()};
  if (!created) {
    throw_sdl_error("cannot create a graphics pipeline");
  }
  return pipelines.emplace(key, std::move(created)).first->second.get();
}

void SdlGpuRenderer::Impl::upload_vertices(SDL_GPUCommandBuffer* commands) {
  const std::span<const SpriteVertex> vertices = recorder.vertices();
  if (vertices.empty()) {
    return;
  }
  const std::size_t needed = vertices.size_bytes();
  if (needed > std::numeric_limits<std::uint32_t>::max()) {
    throw RenderError{"too many quads in one frame"};
  }
  const auto bytes = static_cast<std::uint32_t>(needed);
  if (bytes > vertex_buffer_bytes) {
    const std::uint32_t capacity =
        std::max({bytes, vertex_buffer_bytes * 2, kMinVertexBufferBytes});
    vertex_buffer = make_buffer(SDL_GPU_BUFFERUSAGE_VERTEX, capacity);
    vertex_buffer_bytes = capacity;
  }
  if (bytes > vertex_upload_bytes) {
    vertex_upload = make_transfer_buffer(SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, vertex_buffer_bytes);
    vertex_upload_bytes = vertex_buffer_bytes;
  }
  // Cycling lets the GPU keep reading last frame's data while this frame's is written.
  void* mapped = SDL_MapGPUTransferBuffer(device.get(), vertex_upload.get(), true);
  if (mapped == nullptr) {
    throw_sdl_error("cannot map a GPU transfer buffer");
  }
  std::memcpy(mapped, vertices.data(), bytes);
  SDL_UnmapGPUTransferBuffer(device.get(), vertex_upload.get());

  SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
  const SDL_GPUTransferBufferLocation source{.transfer_buffer = vertex_upload.get(), .offset = 0};
  const SDL_GPUBufferRegion destination{.buffer = vertex_buffer.get(), .offset = 0, .size = bytes};
  SDL_UploadToGPUBuffer(copy, &source, &destination, true);
  SDL_EndGPUCopyPass(copy);
}

std::uint32_t SdlGpuRenderer::Impl::record_passes(SDL_GPUCommandBuffer* commands,
                                                  SDL_GPUTexture* swapchain, Size swapchain_size) {
  std::uint32_t draw_calls = 0;
  const std::span<const Batch> batches = recorder.batches();
  const std::span<const std::byte> uniform_bytes = recorder.uniform_bytes();
  for (const RecordedPass& recorded : recorder.passes()) {
    SDL_GPUTexture* target = nullptr;
    SDL_GPUTextureFormat format = kTextureFormat;
    Size target_size{};
    if (recorded.pass.target == kScreen) {
      if (swapchain == nullptr) {
        continue;  // The window is minimized or hidden: nothing to draw to this frame.
      }
      target = swapchain;
      format = SDL_GetGPUSwapchainTextureFormat(device.get(), window->window());
      target_size = swapchain_size;
    } else {
      const Texture& target_texture = texture(recorded.pass.target);
      target = target_texture.texture.get();
      target_size = target_texture.size;
    }

    SDL_GPUColorTargetInfo color_target{};
    color_target.texture = target;
    color_target.load_op = recorded.pass.clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    color_target.store_op = SDL_GPU_STOREOP_STORE;
    if (recorded.pass.clear) {
      color_target.clear_color = premultiplied(*recorded.pass.clear);
    }
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color_target, 1, nullptr);
    if (pass == nullptr) {
      throw_sdl_error("cannot begin a render pass");
    }
    if (recorded.batch_count != 0) {
      const std::array<float, 4> pass_uniforms{static_cast<float>(target_size.width),
                                               static_cast<float>(target_size.height), 0, 0};
      SDL_PushGPUVertexUniformData(commands, 0, pass_uniforms.data(),
                                   static_cast<std::uint32_t>(sizeof(pass_uniforms)));
      const SDL_GPUBufferBinding vertex_binding{.buffer = vertex_buffer.get(), .offset = 0};
      SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);
      const SDL_GPUBufferBinding index_binding{.buffer = index_buffer.get(), .offset = 0};
      SDL_BindGPUIndexBuffer(pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
    }
    SDL_GPUGraphicsPipeline* bound = nullptr;
    for (const Batch& batch : batches.subspan(recorded.first_batch, recorded.batch_count)) {
      SDL_GPUGraphicsPipeline* wanted = pipeline(batch.shader, batch.blend, format);
      if (wanted != bound) {
        SDL_BindGPUGraphicsPipeline(pass, wanted);
        bound = wanted;
      }
      const Texture& source = texture(batch.texture);
      const SDL_GPUTextureSamplerBinding binding{.texture = source.texture.get(),
                                                 .sampler = source.filter == TextureFilter::kNearest
                                                                ? nearest_sampler.get()
                                                                : linear_sampler.get()};
      SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
      if (batch.uniform_size != 0) {
        const std::span<const std::byte> uniforms =
            uniform_bytes.subspan(batch.uniform_offset, batch.uniform_size);
        SDL_PushGPUFragmentUniformData(commands, 0, uniforms.data(),
                                       static_cast<std::uint32_t>(uniforms.size()));
      }
      std::uint32_t first = batch.first_quad;
      std::uint32_t remaining = batch.quad_count;
      while (remaining != 0) {
        const std::uint32_t count = std::min(remaining, kMaxQuadsPerDraw);
        SDL_DrawGPUIndexedPrimitives(pass, count * kIndicesPerQuad, 1, 0,
                                     static_cast<std::int32_t>(first * kVerticesPerQuad), 0);
        ++draw_calls;
        first += count;
        remaining -= count;
      }
    }
    SDL_EndGPURenderPass(pass);
  }
  return draw_calls;
}

void SdlGpuRenderer::Impl::release_destroyed() {
  // SDL defers the actual release until the GPU no longer uses the objects.
  for (const TextureId id : textures_to_destroy) {
    textures.erase(id.value);
  }
  textures_to_destroy.clear();
  for (const ShaderId id : shaders_to_destroy) {
    std::erase_if(pipelines,
                  [&](const auto& entry) { return std::get<0>(entry.first) == id.value; });
    custom_shaders.erase(id.value);
  }
  shaders_to_destroy.clear();
}

SdlGpuRenderer::SdlGpuRenderer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

SdlGpuRenderer::~SdlGpuRenderer() = default;

std::unique_ptr<SdlGpuRenderer> SdlGpuRenderer::create_for_window(SdlPlatform& platform,
                                                                  const RendererConfig& config) {
  return std::unique_ptr<SdlGpuRenderer>{
      new SdlGpuRenderer{std::make_unique<Impl>(config, nullptr, platform.sdl_window())}};
}

std::unique_ptr<SdlGpuRenderer> SdlGpuRenderer::create_headless(const RendererConfig& config) {
  return std::unique_ptr<SdlGpuRenderer>{new SdlGpuRenderer{
      std::make_unique<Impl>(config, std::make_unique<VideoSubsystem>(), nullptr)}};
}

std::string_view SdlGpuRenderer::backend_name() const {
  const char* name = SDL_GetGPUDeviceDriver(impl_->device.get());
  return name != nullptr ? std::string_view{name} : std::string_view{"unknown"};
}

TextureId SdlGpuRenderer::create_texture(const Image& image, TextureFilter filter) {
  const std::size_t expected_bytes = static_cast<std::size_t>(std::max(image.size.width, 0)) *
                                     static_cast<std::size_t>(std::max(image.size.height, 0)) *
                                     kBytesPerPixel;
  if (image.pixels.size() != expected_bytes) {
    throw RenderError{"image data does not match its size"};
  }
  const TextureId id = impl_->add_texture(image.size, filter, false);
  SDL_GPUDevice* device = impl_->device.get();
  const auto bytes = static_cast<std::uint32_t>(expected_bytes);
  const GpuPtr<SDL_GPUTransferBuffer> upload =
      impl_->make_transfer_buffer(SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes);
  void* mapped = SDL_MapGPUTransferBuffer(device, upload.get(), false);
  if (mapped == nullptr) {
    throw_sdl_error("cannot map a GPU transfer buffer");
  }
  std::memcpy(mapped, image.pixels.data(), bytes);
  SDL_UnmapGPUTransferBuffer(device, upload.get());

  SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
  if (commands == nullptr) {
    throw_sdl_error("cannot start GPU commands");
  }
  SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
  SDL_GPUTextureTransferInfo source{};
  source.transfer_buffer = upload.get();
  source.pixels_per_row = static_cast<std::uint32_t>(image.size.width);
  source.rows_per_layer = static_cast<std::uint32_t>(image.size.height);
  SDL_GPUTextureRegion destination{};
  destination.texture = impl_->texture(id).texture.get();
  destination.w = static_cast<std::uint32_t>(image.size.width);
  destination.h = static_cast<std::uint32_t>(image.size.height);
  destination.d = 1;
  SDL_UploadToGPUTexture(copy, &source, &destination, false);
  SDL_EndGPUCopyPass(copy);
  if (!SDL_SubmitGPUCommandBuffer(commands)) {
    throw_sdl_error("cannot submit GPU commands");
  }
  return id;
}

TextureId SdlGpuRenderer::create_render_target(Size size, TextureFilter filter) {
  const TextureId id = impl_->add_texture(size, filter, true);
  // Start transparent black, as documented, rather than with whatever the memory held.
  SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(impl_->device.get());
  if (commands == nullptr) {
    throw_sdl_error("cannot start GPU commands");
  }
  SDL_GPUColorTargetInfo color_target{};
  color_target.texture = impl_->texture(id).texture.get();
  color_target.load_op = SDL_GPU_LOADOP_CLEAR;
  color_target.store_op = SDL_GPU_STOREOP_STORE;
  color_target.clear_color = SDL_FColor{.r = 0, .g = 0, .b = 0, .a = 0};
  SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color_target, 1, nullptr);
  SDL_EndGPURenderPass(pass);
  if (!SDL_SubmitGPUCommandBuffer(commands)) {
    throw_sdl_error("cannot submit GPU commands");
  }
  return id;
}

ShaderId SdlGpuRenderer::create_shader(const ShaderBytecode& fragment) {
  if (fragment.samplers > 1 || fragment.uniform_buffers > 1) {
    throw RenderError{
        "a custom fragment shader may use one texture and one constant buffer, as declared in "
        "shaders/vnmaki.hlsli"};
  }
  // Every fragment shader gets the drawn texture in slot 0, used or not.
  ShaderBytecode bytecode = fragment;
  bytecode.samplers = 1;
  Impl::Shader shader{.shader = impl_->make_shader(bytecode, SDL_GPU_SHADERSTAGE_FRAGMENT),
                      .takes_uniforms = fragment.uniform_buffers == 1};
  const ShaderId id{impl_->next_shader++};
  impl_->custom_shaders.emplace(id.value, std::move(shader));
  return id;
}

void SdlGpuRenderer::destroy_texture(TextureId texture) {
  if (impl_->textures.contains(texture.value)) {
    impl_->textures_to_destroy.push_back(texture);
    if (!impl_->in_frame) {
      impl_->release_destroyed();
    }
  }
}

void SdlGpuRenderer::destroy_shader(ShaderId shader) {
  if (impl_->custom_shaders.contains(shader.value)) {
    impl_->shaders_to_destroy.push_back(shader);
    if (!impl_->in_frame) {
      impl_->release_destroyed();
    }
  }
}

Size SdlGpuRenderer::texture_size(TextureId texture) const { return impl_->texture(texture).size; }

Size SdlGpuRenderer::screen_size() const {
  if (!impl_->window) {
    return Size{};
  }
  Size size;
  SDL_GetWindowSizeInPixels(impl_->window->window(), &size.width, &size.height);
  return size;
}

void SdlGpuRenderer::begin_frame() {
  if (impl_->in_frame) {
    throw RenderError{"begin_frame() inside a frame"};
  }
  impl_->recorder.reset();
  impl_->in_frame = true;
}

void SdlGpuRenderer::begin_pass(const RenderPass& pass) {
  if (!impl_->in_frame) {
    throw RenderError{"begin_pass() outside a frame"};
  }
  if (pass.target == kScreen) {
    if (!impl_->window) {
      throw RenderError{"this renderer has no screen to draw to"};
    }
  } else if (!impl_->texture(pass.target).render_target) {
    throw RenderError{"texture " + std::to_string(pass.target.value) + " is not a render target"};
  }
  impl_->recorder.begin_pass(pass);
}

void SdlGpuRenderer::draw(std::span<const Quad> quads, const DrawState& state) {
  if (!impl_->in_frame || !impl_->recorder.in_pass()) {
    throw RenderError{"draw() outside a render pass"};
  }
  static_cast<void>(impl_->texture(state.texture));
  if (state.texture == impl_->recorder.current_pass().target) {
    throw RenderError{"a pass cannot draw its own target"};
  }
  bool takes_uniforms = false;
  if (state.shader != kSpriteShader) {
    const auto found = impl_->custom_shaders.find(state.shader.value);
    if (found == impl_->custom_shaders.end()) {
      throw RenderError{"unknown shader id " + std::to_string(state.shader.value)};
    }
    takes_uniforms = found->second.takes_uniforms;
  }
  if (takes_uniforms == state.uniforms.empty()) {
    throw RenderError{takes_uniforms ? "the shader needs uniforms but the draw has none"
                                     : "the draw has uniforms but the shader takes none"};
  }
  impl_->recorder.add(quads, state);
}

FrameStats SdlGpuRenderer::end_frame() {
  if (!impl_->in_frame) {
    throw RenderError{"end_frame() outside a frame"};
  }
  impl_->in_frame = false;
  FrameStats stats;
  stats.quads = impl_->recorder.quad_count();

  SDL_GPUDevice* device = impl_->device.get();
  SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
  if (commands == nullptr) {
    impl_->release_destroyed();
    throw_sdl_error("cannot start GPU commands");
  }
  try {
    impl_->upload_vertices(commands);
    const std::span<const RecordedPass> passes = impl_->recorder.passes();
    SDL_GPUTexture* swapchain = nullptr;
    Size swapchain_size;
    if (std::ranges::any_of(passes,
                            [](const RecordedPass& p) { return p.pass.target == kScreen; })) {
      std::uint32_t width = 0;
      std::uint32_t height = 0;
      if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, impl_->window->window(), &swapchain,
                                                 &width, &height)) {
        throw_sdl_error("cannot get the window's back buffer");
      }
      swapchain_size = Size{.width = static_cast<int>(width), .height = static_cast<int>(height)};
    }
    stats.draw_calls = impl_->record_passes(commands, swapchain, swapchain_size);
    stats.passes =
        static_cast<std::uint32_t>(std::ranges::count_if(passes, [&](const RecordedPass& p) {
          return p.pass.target != kScreen || swapchain != nullptr;
        }));
  } catch (...) {
    SDL_CancelGPUCommandBuffer(commands);
    impl_->release_destroyed();
    throw;
  }
  const bool submitted = SDL_SubmitGPUCommandBuffer(commands);
  impl_->release_destroyed();
  if (!submitted) {
    throw_sdl_error("cannot submit GPU commands");
  }
  return stats;
}

Image SdlGpuRenderer::read_pixels(TextureId texture) {
  if (impl_->in_frame) {
    throw RenderError{"read_pixels() inside a frame"};
  }
  const Impl::Texture& source = impl_->texture(texture);
  const auto width = static_cast<std::uint32_t>(source.size.width);
  const auto height = static_cast<std::uint32_t>(source.size.height);
  const std::uint32_t bytes = width * height * kBytesPerPixel;
  SDL_GPUDevice* device = impl_->device.get();
  const GpuPtr<SDL_GPUTransferBuffer> download =
      impl_->make_transfer_buffer(SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, bytes);

  SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
  if (commands == nullptr) {
    throw_sdl_error("cannot start GPU commands");
  }
  SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
  SDL_GPUTextureRegion region{};
  region.texture = source.texture.get();
  region.w = width;
  region.h = height;
  region.d = 1;
  SDL_GPUTextureTransferInfo destination{};
  destination.transfer_buffer = download.get();
  destination.pixels_per_row = width;
  destination.rows_per_layer = height;
  SDL_DownloadFromGPUTexture(copy, &region, &destination);
  SDL_EndGPUCopyPass(copy);
  SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
  if (fence == nullptr) {
    throw_sdl_error("cannot submit GPU commands");
  }
  const bool finished = SDL_WaitForGPUFences(device, true, &fence, 1);
  SDL_ReleaseGPUFence(device, fence);
  if (!finished) {
    throw_sdl_error("cannot wait for the GPU");
  }

  const void* mapped = SDL_MapGPUTransferBuffer(device, download.get(), false);
  if (mapped == nullptr) {
    throw_sdl_error("cannot map a GPU transfer buffer");
  }
  Image image;
  image.size = source.size;
  image.pixels.resize(bytes);
  std::memcpy(image.pixels.data(), mapped, bytes);
  SDL_UnmapGPUTransferBuffer(device, download.get());
  return image;
}

}  // namespace vnmaki
