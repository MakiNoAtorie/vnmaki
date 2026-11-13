#pragma once

// Dear ImGui on the SDL GPU renderer. This header includes neither ImGui nor SDL; the context it
// makes is ImGui's current one, which overlay.cpp uses directly.

#include "engine/platform/geometry.hpp"
#include "engine/render/sdl_gpu_renderer.hpp"

namespace vnmaki {

/// Owns the ImGui context and its GPU objects, and draws the frame over the last pass of the
/// renderer's frame. Destroy it before the renderer. Only one may exist.
class ImguiBackend {
 public:
  /// Throws RenderError if the backend cannot start.
  explicit ImguiBackend(SdlGpuRenderer& renderer);
  ~ImguiBackend();
  ImguiBackend(const ImguiBackend&) = delete;
  ImguiBackend& operator=(const ImguiBackend&) = delete;
  ImguiBackend(ImguiBackend&&) = delete;
  ImguiBackend& operator=(ImguiBackend&&) = delete;

  /// Starts an ImGui frame for a screen of `pixels`; `seconds` since the last one.
  void begin_frame(Size pixels, double seconds);
  /// Ends the frame. The renderer draws it in its end_frame(). Without it, nothing is drawn.
  void end_frame();

 private:
  void draw(const GpuOverlayTarget& target);

  SdlGpuRenderer* renderer_;
  bool ready_ = false;
};

}  // namespace vnmaki
