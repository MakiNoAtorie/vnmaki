#include "cli/story_window.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "engine/anim/wait_presenter.hpp"
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
#include "engine/ui/screen_presenter.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/ui/ui_renderer.hpp"
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
    // The fallback chain of the source language (the first in project.toml).
    std::vector<Font> fallback_fonts;
    if (!config.languages.empty()) {
      if (const auto chain = config.font_fallbacks.find(config.languages.front());
          chain != config.font_fallbacks.end()) {
        fallback_fonts.reserve(chain->second.size());
        for (const std::string& path : chain->second) {
          fallback_fonts.push_back(load_font(vfs, path));
        }
      }
    }
    FontSet fonts{.base = {.regular = &font}};
    for (const Font& fallback : fallback_fonts) {
      fonts.fallbacks.push_back(&fallback);
    }
    Scene scene{vfs, virtual_size};
    UiHost ui{virtual_size};
    ScriptRuntime runtime{vfs, {.scene_state = &scene, .scene = &scene, .screens = &ui}};
    runtime.load_scripts();

    SdlPlatform platform{WindowConfig{.title = config.title, .size = kDefaultWindowSize}};
    const std::unique_ptr<SdlGpuRenderer> renderer = SdlGpuRenderer::create_for_window(platform);
    JobSystem jobs;
    const TextureId canvas = renderer->create_render_target(virtual_size, TextureFilter::kLinear);
    UiRenderer ui_renderer{*renderer, fonts, &vfs};
    ui.set_text_measure(ui_renderer.text_measure());
    SayBoxView say_box{*renderer, std::move(fonts), SayBoxStyle::for_resolution(virtual_size)};

    runtime.start("start");
    ScreenPresenter screens{runtime, ui};
    WaitPresenter waits{runtime, scene};
    DialoguePresenter presenter{
        runtime,
        [&](const DialogueLine& line) { return say_box.glyph_count(line); },
        kDefaultCharactersPerSecond,
        [&](const DialogueLine& line, double speed, double elapsed) {
          return say_box.revealed(line, speed, elapsed);
        }};

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

    // A click, key or button ends a wait for tweens first (skip mode will do the same).
    const auto advance = [&] {
      if (!waits.skip()) {
        presenter.advance();
      }
    };
    MainLoopHooks hooks;
    hooks.on_event = [&](const InputEvent& event) {
      if (screens.handle_event(event, viewport())) {
        return;
      }
      if (const auto* key = std::get_if<KeyEvent>(&event)) {
        if (key->pressed && !key->repeat && is_advance_key(key->key)) {
          advance();
        }
      } else if (const auto* click = std::get_if<PointerButtonEvent>(&event)) {
        if (click->pressed && click->button == MouseButton::kLeft &&
            inside_canvas(click->position)) {
          const std::optional<DialogueLine>& line = presenter.line();
          const std::string link =
              line ? say_box.link_at(*line, window_to_virtual(viewport(), click->position)) : "";
          if (link.empty() || !runtime.activate_link(link)) {
            advance();
          }
        }
      } else if (const auto* button = std::get_if<GamepadButtonEvent>(&event)) {
        if (button->pressed && button->button == GamepadButton::kSouth) {
          advance();
        }
      }
    };
    hooks.update = [&](Duration step) {
      const double seconds = std::chrono::duration<double>(step).count();
      presenter.update(seconds);
      scene.update(seconds);
      waits.update();
      screens.sync();
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
      const std::vector<const UiNode*> screen_roots = ui.roots();
      if (placed.scale > 0) {
        for (const UiNode* root : screen_roots) {
          ui_renderer.prepare(*root, placed.scale);
        }
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
      if (placed.scale > 0) {
        for (const UiNode* root : screen_roots) {
          ui_renderer.draw(*root, placed.scale, {.x = placed.rect.x, .y = placed.rect.y});
        }
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
