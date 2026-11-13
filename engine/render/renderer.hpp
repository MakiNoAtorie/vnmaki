#pragma once

// The renderer interface (docs/SPEC.md section 9). Engine code draws only through IRenderer, so
// another backend (WebGPU/WebGL for the web build) can replace the SDL GPU one
// (engine/render/sdl_gpu_renderer.hpp).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

#include "engine/platform/geometry.hpp"
#include "engine/render/image.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/shader.hpp"

namespace vnmaki {

/// A colour with straight (not premultiplied) alpha; channels in [0, 1].
struct Color {
  float r = 0;
  float g = 0;
  float b = 0;
  float a = 1;

  bool operator==(const Color&) const = default;
};

/// A colour from 8-bit channels.
[[nodiscard]] constexpr Color rgba8(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                    std::uint8_t a = UINT8_MAX) {
  constexpr auto kMax = static_cast<float>(UINT8_MAX);
  return Color{.r = static_cast<float>(r) / kMax,
               .g = static_cast<float>(g) / kMax,
               .b = static_cast<float>(b) / kMax,
               .a = static_cast<float>(a) / kMax};
}

inline constexpr Color kWhite{.r = 1, .g = 1, .b = 1, .a = 1};

/// Thrown when the GPU cannot be used (no device, out of memory) or the renderer is misused
/// (an unknown texture, drawing outside a pass). The message gives the reason.
class RenderError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Names a texture or render target a renderer owns. The zero value names none.
struct TextureId {
  std::uint32_t value = 0;

  bool operator==(const TextureId&) const = default;
};

/// As a pass target: the screen (the window's back buffer).
inline constexpr TextureId kScreen{};

/// Names a custom fragment shader a renderer owns. The zero value is the built-in sprite shader.
struct ShaderId {
  std::uint32_t value = 0;

  bool operator==(const ShaderId&) const = default;
};

inline constexpr ShaderId kSpriteShader{};

enum class TextureFilter : std::uint8_t {
  kLinear,   ///< Smooth when scaled: photos, painted backgrounds.
  kNearest,  ///< Hard texel edges: pixel art.
};

/// How a quad's colour combines with what is already in the target. Colours are premultiplied
/// throughout, so these are the premultiplied-alpha forms.
enum class BlendMode : std::uint8_t {
  kAlpha,     ///< Normal transparency: source over destination.
  kAdditive,  ///< Adds light: glows, sparkles.
  kReplace,   ///< Overwrites colour and alpha, ignoring what was there.
};

/// Texture coordinates of a whole texture: (0, 0) is its top-left corner, (1, 1) its
/// bottom-right.
inline constexpr RectF kWholeTexture{.x = 0, .y = 0, .width = 1, .height = 1};

/// A textured quad. Its corners may form any convex quadrilateral, so a scene graph can rotate,
/// scale and skew sprites before drawing them.
struct Quad {
  /// Where the texture's top-left, top-right, bottom-right and bottom-left corners land, in the
  /// pass target's pixels (origin at the top-left, y down).
  std::array<PointF, 4> corners{};
  /// The part of the texture to show.
  RectF uv = kWholeTexture;
  /// Multiplies the texture's colour (straight alpha; white leaves it unchanged).
  Color tint = kWhite;
};

/// An axis-aligned quad covering `destination` (target pixels).
[[nodiscard]] Quad quad_from_rect(RectF destination, RectF uv = kWholeTexture, Color tint = kWhite);

/// What a draw call draws with. Consecutive quads with equal draw states are batched into one
/// GPU draw call.
struct DrawState {
  TextureId texture{};
  ShaderId shader = kSpriteShader;
  BlendMode blend = BlendMode::kAlpha;
  /// The bytes of the custom shader's constant buffer (register(b0, space3)); the renderer copies
  /// them. Required by a shader that declares the buffer, which reads as many bytes as it
  /// declares (HLSL packs constants in 16-byte rows); must be empty otherwise.
  std::span<const std::byte> uniforms{};
  /// The textures of sampler slots 1 and 2 (`sample_texture1` and `sample_texture2` in
  /// shaders/vnmaki.hlsli), for a custom shader that reads more than one texture, e.g. a
  /// transition. An id of zero binds `texture` there. Ignored by the built-in sprite shader.
  std::array<TextureId, 2> more_textures{};
};

/// One mesh draw in a pass with a depth buffer (RenderPass::depth). The vertex shader takes a
/// vertex to world space with `model` and then to clip space with `view_projection`. The fragment
/// shader gets world positions and normals (turned by the upper 3x3 of `model`, so its scale must
/// be uniform) and is lit by itself: shaders/mesh.hlsli says what it receives.
struct MeshDraw {
  MeshId mesh{};
  /// World space to clip space, with depth from 0 (near) to 1 (far) and y up.
  Mat4 view_projection{};
  /// Model space to world space.
  Mat4 model{};
  /// The base colour texture (`sample_base` in shaders/mesh.hlsli). Required.
  TextureId texture{};
  /// A fragment shader made with create_shader from a shader that includes shaders/mesh.hlsli.
  /// Required: there is no default mesh shader.
  ShaderId shader{};
  /// The bytes of the shader's constant buffer (register(b0, space3)); as for DrawState.
  std::span<const std::byte> uniforms{};
};

/// The most textures a custom fragment shader may read: `texture` and `DrawState::more_textures`.
inline constexpr std::uint32_t kMaxShaderTextures = 3;

/// One render pass: a run of draws into one target.
struct RenderPass {
  /// A render target, or kScreen.
  TextureId target = kScreen;
  /// Clears the target to this colour first; without it the pass draws over what is there.
  std::optional<Color> clear{};
  /// The pass has a depth buffer (cleared to far). It draws meshes (draw_mesh) and no quads, and
  /// its target must be a render target.
  bool depth = false;
};

/// Work the GPU did for one frame.
struct FrameStats {
  std::uint32_t passes = 0;
  std::uint32_t quads = 0;
  /// GPU draw calls. Batching keeps this low: consecutive quads that share a draw state cost one.
  std::uint32_t draw_calls = 0;
};

/// The GPU memory of textures and render targets, as the renderer counted it (the profiler shows
/// it).
struct TextureMemory {
  std::uint64_t texture_bytes = 0;
  std::uint64_t target_bytes = 0;
  std::uint32_t textures = 0;
  std::uint32_t targets = 0;

  bool operator==(const TextureMemory&) const = default;
};

/// Draws textured quads into render targets and the screen.
///
/// A frame is begin_frame(), then passes (begin_pass() followed by draws), then end_frame(),
/// which submits the work and presents the screen. Resources may be created and destroyed at
/// any time; destroying one that the current frame uses takes effect after the frame. Coordinates
/// are pixels of the pass target, origin at the top-left. All colours are premultiplied on the
/// GPU: textures from Image already are, and tints are converted.
///
/// Renderers are not thread-safe: use one from the thread that created it (the main thread).
class IRenderer {
 public:
  IRenderer() = default;
  IRenderer(const IRenderer&) = delete;
  IRenderer& operator=(const IRenderer&) = delete;
  IRenderer(IRenderer&&) = delete;
  IRenderer& operator=(IRenderer&&) = delete;
  virtual ~IRenderer() = default;

  /// The backend's name, for logs (e.g. "vulkan", "direct3d12", "metal").
  [[nodiscard]] virtual std::string_view backend_name() const = 0;

  /// Uploads `image` (premultiplied RGBA, see Image) as a texture. Throws RenderError.
  [[nodiscard]] virtual TextureId create_texture(const Image& image, TextureFilter filter) = 0;

  /// Creates a render target: a texture that passes draw into and draws can then use, e.g. to
  /// compose a layer once and shade it as a whole. Its contents start transparent black.
  [[nodiscard]] virtual TextureId create_render_target(Size size, TextureFilter filter) = 0;

  /// Creates a custom fragment shader (see shaders/vnmaki.hlsli for what it receives). Throws
  /// RenderError if it has no bytecode for this backend or uses resources it may not.
  [[nodiscard]] virtual ShaderId create_shader(const ShaderBytecode& fragment) = 0;

  /// Uploads a triangle mesh: `indices` are triples into `vertices`. Throws RenderError for an
  /// index out of range or a count that is not a multiple of 3. The default throws, for renderers
  /// that cannot draw meshes.
  [[nodiscard]] virtual MeshId create_mesh(std::span<const MeshVertex> vertices,
                                           std::span<const std::uint32_t> indices) {
    static_cast<void>(vertices);
    static_cast<void>(indices);
    throw RenderError{"this renderer cannot draw meshes"};
  }
  /// Releases a mesh. Unknown ids are ignored.
  virtual void destroy_mesh(MeshId /*mesh*/) {}

  /// Releases a texture or render target. Unknown ids are ignored.
  virtual void destroy_texture(TextureId texture) = 0;
  /// Releases a custom shader. Unknown ids are ignored.
  virtual void destroy_shader(ShaderId shader) = 0;

  /// The size of a texture or render target in pixels. Throws RenderError for an unknown id.
  [[nodiscard]] virtual Size texture_size(TextureId texture) const = 0;

  /// The screen's size in pixels (the window's pixel size), or 0x0 without a screen.
  [[nodiscard]] virtual Size screen_size() const = 0;

  virtual void begin_frame() = 0;
  /// Starts a pass, ending the previous one. Throws RenderError outside a frame, for an unknown
  /// target, or for kScreen on a renderer without a screen.
  virtual void begin_pass(const RenderPass& pass) = 0;
  /// Draws quads in the current pass. Throws RenderError outside a pass, for an unknown texture or
  /// shader, for the pass's own target as the texture, or for uniforms the shader does not take.
  virtual void draw(std::span<const Quad> quads, const DrawState& state) = 0;
  void draw(const Quad& quad, const DrawState& state) { draw(std::span{&quad, 1}, state); }
  /// Draws a mesh in the current pass, which must have a depth buffer. Throws RenderError outside
  /// such a pass, for an unknown mesh, texture or shader (the sprite shader is not one), or for
  /// uniforms the shader does not take. The default throws.
  virtual void draw_mesh(const MeshDraw& draw) {
    static_cast<void>(draw);
    throw RenderError{"this renderer cannot draw meshes"};
  }
  /// Submits the frame to the GPU and presents the screen, if a pass drew to it.
  virtual FrameStats end_frame() = 0;

  /// Copies a texture or render target back from the GPU (waiting for the GPU to finish), e.g.
  /// for screenshots and tests. Call it between frames. Throws RenderError.
  [[nodiscard]] virtual Image read_pixels(TextureId texture) = 0;
};

}  // namespace vnmaki
