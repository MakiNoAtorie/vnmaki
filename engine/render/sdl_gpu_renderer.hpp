#pragma once

// The SDL3 GPU backend of IRenderer: Vulkan, Direct3D 12 or Metal, whichever SDL picks for the
// platform. This header does not include SDL.

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "engine/render/renderer.hpp"

namespace vnmaki {

class SdlPlatform;

struct RendererConfig {
  /// Turn on the GPU driver's validation layers, where installed (slow).
  bool debug = false;
  /// Present at the display's refresh rate. Off presents as soon as a frame is ready.
  bool vsync = true;
};

/// IRenderer on SDL3's GPU API.
///
/// SDL chooses the driver: Vulkan on Linux, Direct3D 12 on Windows, Metal on macOS (software
/// drivers such as Mesa's lavapipe and Windows' WARP count). The SDL_GPU_DRIVER environment
/// variable (or SDL hint) overrides the choice, e.g. SDL_GPU_DRIVER=vulkan on Windows.
class SdlGpuRenderer final : public IRenderer {
 public:
  /// A renderer for `platform`'s window; its screen is the window. Destroy it before the
  /// platform. Throws RenderError if no supported GPU device can be created.
  [[nodiscard]] static std::unique_ptr<SdlGpuRenderer> create_for_window(
      SdlPlatform& platform, const RendererConfig& config = {});

  /// A renderer without a screen, drawing only into render targets: tools, thumbnails and tests.
  /// It starts SDL's video subsystem itself (on Linux without a display, set
  /// SDL_VIDEO_DRIVER=offscreen). Throws RenderError if no supported GPU device can be created.
  [[nodiscard]] static std::unique_ptr<SdlGpuRenderer> create_headless(
      const RendererConfig& config = {});

  ~SdlGpuRenderer() override;
  SdlGpuRenderer(const SdlGpuRenderer&) = delete;
  SdlGpuRenderer& operator=(const SdlGpuRenderer&) = delete;
  SdlGpuRenderer(SdlGpuRenderer&&) = delete;
  SdlGpuRenderer& operator=(SdlGpuRenderer&&) = delete;

  [[nodiscard]] std::string_view backend_name() const override;
  [[nodiscard]] TextureId create_texture(const Image& image, TextureFilter filter) override;
  [[nodiscard]] TextureId create_render_target(Size size, TextureFilter filter) override;
  [[nodiscard]] ShaderId create_shader(const ShaderBytecode& fragment) override;
  void destroy_texture(TextureId texture) override;
  void destroy_shader(ShaderId shader) override;
  [[nodiscard]] Size texture_size(TextureId texture) const override;
  [[nodiscard]] Size screen_size() const override;
  void begin_frame() override;
  void begin_pass(const RenderPass& pass) override;
  using IRenderer::draw;
  void draw(std::span<const Quad> quads, const DrawState& state) override;
  FrameStats end_frame() override;
  [[nodiscard]] Image read_pixels(TextureId texture) override;

 private:
  struct Impl;
  explicit SdlGpuRenderer(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace vnmaki
