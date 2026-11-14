// The SDL GPU renderer on a real window: the screen pass. On Linux the window comes from SDL's
// offscreen video driver, so nothing is shown and the back buffer cannot be read back; the test
// checks what the renderer reports instead.

#include <memory>

#include <doctest/doctest.h>

#include "engine/platform/geometry.hpp"
#include "engine/platform/sdl_platform.hpp"
#include "engine/platform/viewport.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/sdl_gpu_renderer.hpp"
#include "tests/render/gpu_test_support.hpp"

TEST_CASE("renderer: a window renderer composes the virtual canvas onto the screen") {
  const vnmaki::WindowConfig window{.title = "vnmaki renderer test",
                                    .size = {.width = 320, .height = 200}};
  // The scene draws at a fixed virtual resolution into a canvas; the canvas is then scaled into
  // the window with letterboxing (engine/platform/viewport.hpp).
  const vnmaki::Size virtual_size{.width = 64, .height = 36};
  const vnmaki::RectF sprite{.x = 8, .y = 8, .width = 16, .height = 16};
  const vnmaki::Color canvas_clear = vnmaki::rgba8(0, 0, 64);

  std::unique_ptr<vnmaki::SdlPlatform> platform;
  try {
    platform = std::make_unique<vnmaki::SdlPlatform>(window);
  } catch (const vnmaki::PlatformError& error) {
    vnmaki::test::skip_without_gpu(error.what());
    return;
  }
  std::unique_ptr<vnmaki::SdlGpuRenderer> renderer;
  try {
    renderer = vnmaki::SdlGpuRenderer::create_for_window(*platform, {.vsync = false});
  } catch (const vnmaki::RenderError& error) {
    vnmaki::test::skip_without_gpu(error.what());
    return;
  }
  const vnmaki::Size screen = renderer->screen_size();
  CHECK(screen == platform->window_metrics().pixels);

  const vnmaki::TextureId canvas =
      renderer->create_render_target(virtual_size, vnmaki::TextureFilter::kLinear);
  const vnmaki::Image white_pixel{.size = {.width = 1, .height = 1},
                                  .pixels = {255, 255, 255, 255}};
  const vnmaki::TextureId white =
      renderer->create_texture(white_pixel, vnmaki::TextureFilter::kNearest);
  const vnmaki::Viewport viewport =
      vnmaki::compute_viewport(virtual_size, screen, vnmaki::ScaleMode::kFit);

  constexpr int kFrames = 3;
  for (int frame = 0; frame < kFrames; ++frame) {
    renderer->begin_frame();
    renderer->begin_pass({.target = canvas, .clear = canvas_clear});
    renderer->draw(vnmaki::quad_from_rect(sprite), {.texture = white});
    renderer->begin_pass({.target = vnmaki::kScreen, .clear = vnmaki::rgba8(0, 0, 0)});
    renderer->draw(vnmaki::quad_from_rect(viewport.rect), {.texture = canvas});
    const vnmaki::FrameStats stats = renderer->end_frame();

    CHECK(stats.quads == 2);
    // A hidden or minimized window may have no back buffer for a frame; then the screen pass
    // is skipped.
    CHECK((stats.passes == 2 || stats.passes == 1));
    CHECK(stats.draw_calls == stats.passes);
  }
  renderer.reset();  // Before the window it draws to.
}
