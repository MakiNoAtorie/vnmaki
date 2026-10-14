#include "cli/story_window.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <variant>

#include "engine/dialogue/dialogue_presenter.hpp"
#include "engine/dialogue/say_box.hpp"
#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/main_loop.hpp"
#include "engine/platform/sdl_platform.hpp"
#include "engine/platform/viewport.hpp"
#include "engine/render/image.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/sdl_gpu_renderer.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/text/text.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki::cli {

namespace {

constexpr int kExitFailure = 1;

/// Paces the loop until a renderer with vsync does: about 60 frames a second.
constexpr Duration kFrameTime = std::chrono::duration_cast<Duration>(std::chrono::seconds{1}) / 60;

bool is_advance_key(Key key) {
  return key == Key::kEnter || key == Key::kKeypadEnter || key == Key::kSpace;
}

}  // namespace

int run_story_window(const Vfs& vfs, const ProjectConfig& config,
                     std::optional<std::uint64_t> max_frames, std::ostream& err) {
  const Size virtual_size{.width = config.resolution.width, .height = config.resolution.height};
  try {
    // Everything that needs no window comes first, so a broken project fails before one opens.
    const Font font = load_font(vfs, kDefaultFontPath);
    Scene scene{vfs, virtual_size};
    ScriptRuntime runtime{vfs, {.scene_state = &scene, .scene = &scene}};
    runtime.load_scripts();

    SdlPlatform platform{WindowConfig{.title = config.title, .size = kDefaultWindowSize}};
    const std::unique_ptr<SdlGpuRenderer> renderer = SdlGpuRenderer::create_for_window(platform);
    JobSystem jobs;
    const TextureId canvas = renderer->create_render_target(virtual_size, TextureFilter::kLinear);
    SayBoxView say_box{*renderer, font, SayBoxStyle::for_resolution(virtual_size)};

    runtime.start("start");
    DialoguePresenter presenter{
        runtime, [&](const DialogueLine& line) { return say_box.glyph_count(line); }};

    SteadyClock clock;
    MainLoop loop{platform, clock, {.min_frame_time = kFrameTime}};
    const auto viewport = [&] {
      return compute_viewport(virtual_size, platform.window_metrics().pixels, ScaleMode::kFit);
    };
    const auto inside_canvas = [&](PointF window_point) {
      const PointF point = window_to_virtual(viewport(), window_point);
      return point.x >= 0 && point.y >= 0 && point.x < static_cast<float>(virtual_size.width) &&
             point.y < static_cast<float>(virtual_size.height);
    };

    MainLoopHooks hooks;
    hooks.on_event = [&](const InputEvent& event) {
      if (const auto* key = std::get_if<KeyEvent>(&event)) {
        if (key->pressed && !key->repeat && is_advance_key(key->key)) {
          presenter.advance();
        }
      } else if (const auto* click = std::get_if<PointerButtonEvent>(&event)) {
        if (click->pressed && click->button == MouseButton::kLeft &&
            inside_canvas(click->position)) {
          presenter.advance();
        }
      } else if (const auto* button = std::get_if<GamepadButtonEvent>(&event)) {
        if (button->pressed && button->button == GamepadButton::kSouth) {
          presenter.advance();
        }
      }
    };
    hooks.update = [&](Duration step) {
      const double seconds = std::chrono::duration<double>(step).count();
      presenter.update(seconds);
      scene.update(seconds);
    };
    hooks.draw = [&](double /*alpha*/) {
      if (presenter.ended()) {
        loop.request_quit();
        return;
      }
      const Viewport placed = viewport();
      const std::optional<DialogueLine>& line = presenter.line();
      scene.prepare(*renderer, jobs);
      if (line && placed.scale > 0) {
        say_box.prepare(*line, placed.scale);
      }
      renderer->begin_frame();
      renderer->begin_pass({.target = canvas, .clear = rgba8(0, 0, 0)});
      scene.draw(*renderer);
      renderer->begin_pass({.target = kScreen, .clear = rgba8(0, 0, 0)});
      renderer->draw(quad_from_rect(placed.rect), {.texture = canvas});
      if (line && placed.scale > 0) {
        say_box.draw(*line, presenter.visible_glyphs(), placed.scale,
                     {.x = placed.rect.x, .y = placed.rect.y});
      }
      static_cast<void>(renderer->end_frame());
    };
    loop.run(hooks, max_frames);

    if (runtime.state() == RunState::kFailed) {
      err << "vnmaki: " << runtime.error() << '\n';
      return kExitFailure;
    }
  } catch (const std::exception& error) {
    err << "vnmaki: " << error.what() << '\n';
    return kExitFailure;
  }
  return 0;
}

}  // namespace vnmaki::cli
