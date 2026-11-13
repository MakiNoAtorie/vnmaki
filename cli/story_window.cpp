#include "cli/story_window.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "engine/anim/wait_presenter.hpp"
#include "engine/audio/audio.hpp"
#include "engine/dialogue/backlog.hpp"
#include "engine/dialogue/dialogue_presenter.hpp"
#include "engine/dialogue/playback.hpp"
#include "engine/dialogue/rollback_input.hpp"
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
#include "engine/saves/persistent_store.hpp"
#include "engine/saves/saves.hpp"
#include "engine/saves/thumbnail.hpp"
#include "engine/scene/scene.hpp"
#include "engine/text/text.hpp"
#include "engine/ui/backlog_screen.hpp"
#include "engine/ui/playback_indicator.hpp"
#include "engine/ui/preferences.hpp"
#include "engine/ui/screen_actions.hpp"
#include "engine/ui/screen_presenter.hpp"
#include "engine/ui/ui_build.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/ui/ui_renderer.hpp"
#include "engine/vfs/vfs.hpp"
#include "engine/video/movie_presenter.hpp"

namespace vnmaki::cli {

namespace {

constexpr int kExitFailure = 1;

/// Paces the loop until a renderer with vsync does: about 60 frames a second.
constexpr Duration kFrameTime = std::chrono::duration_cast<Duration>(std::chrono::seconds{1}) / 60;

/// The organization part of the user-data path, where saves and the persistent file go.
constexpr std::string_view kUserDataOrganization = "VNMaki";

bool is_advance_key(Key key) {
  return key == Key::kEnter || key == Key::kKeypadEnter || key == Key::kSpace;
}

// The system output; if there is none, a device that plays to nowhere, so that a game with no
// sound card still runs. Builds without threads mix by hand (Audio::update does it).
std::unique_ptr<Audio> make_audio(const Vfs& vfs, std::ostream& err) {
  if (Audio::device_supported()) {
    try {
      return std::make_unique<Audio>(vfs, AudioOptions{.output = AudioOutput::kDevice});
    } catch (const AudioError& error) {
      err << "vnmaki: no audio output (" << error.what() << "); playing without sound\n";
      return std::make_unique<Audio>(vfs, AudioOptions{.output = AudioOutput::kNullDevice});
    }
  }
  return std::make_unique<Audio>(vfs);
}

}  // namespace

int run_story_window(const Vfs& vfs, const ProjectConfig& config,
                     std::optional<std::uint64_t> max_frames,
                     const std::optional<std::filesystem::path>& data_directory_override,
                     std::ostream& err) {
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
    // The audio comes first, so that it outlives the scene: a movie stops its audio when it goes.
    const std::unique_ptr<Audio> audio = make_audio(vfs, err);
    Scene scene{vfs, virtual_size};
    scene.set_movie_audio(audio.get());
    scene.set_voice_level(audio.get());
    CompositeSceneState scene_state{{&scene, audio.get()}};
    UiHost ui{virtual_size};
    ScriptRuntime runtime{vfs,
                          {.rollback_depth = config.rollback_depth,
                           .scene_state = &scene_state,
                           .scene = &scene,
                           .screens = &ui,
                           .audio = audio.get(),
                           .canvas_width = virtual_size.width,
                           .canvas_height = virtual_size.height}};
    ui.set_bindings(&runtime);
    const auto warn = [&err](std::string_view message) { err << "vnmaki: " << message << '\n'; };
    const std::filesystem::path data_directory =
        data_directory_override ? *data_directory_override
                                : user_data_directory(kUserDataOrganization, config.title);
    PersistentStore persistent{runtime, data_directory, warn};
    persistent.load();  // before the scripts, so that their defaults see the saved values
    runtime.load_scripts();

    SdlPlatform platform{WindowConfig{.title = config.title, .size = kDefaultWindowSize}};
    ui.set_platform(&platform);  // text input is on only while an `input` node has the keys
    const std::unique_ptr<SdlGpuRenderer> renderer = SdlGpuRenderer::create_for_window(platform);
    JobSystem jobs;
    const TextureId canvas = renderer->create_render_target(virtual_size, TextureFilter::kLinear);
    UiRenderer ui_renderer{*renderer, fonts, &vfs};
    ui.set_text_measure(ui_renderer.text_measure());
    SayBoxView say_box{*renderer, std::move(fonts), SayBoxStyle::for_resolution(virtual_size)};
    // The say box is the Lua screen `say`; if it fails the built-in box is drawn.
    say_box.set_screen_source(
        [&](const DialogueLine& line) -> std::optional<UiNode> {
          ScreenNodeDef args{.kind = "args",
                             .props = {{"speaker", line.speaker}, {"text", line.text}}};
          if (line.color) {
            args.props.emplace_back("color", std::format("#{:06x}", *line.color));
          }
          ScreenNodeDef tree;
          if (!runtime.build_screen("say", args, tree).empty()) {
            return std::nullopt;
          }
          try {
            return build_ui_tree(tree);
          } catch (const UiBuildError&) {
            return std::nullopt;
          }
        },
        virtual_size);

    Saves saves{runtime,
                {.directory = data_directory,
                 .game_version = config.version,
                 .auto_save = config.auto_save,
                 .thumbnail = [&] { return capture_thumbnail(*renderer, canvas); },
                 .on_warning = warn}};
    runtime.start("start");
    saves.poll();
    ScreenPresenter screens{runtime, ui};
    WaitPresenter waits{runtime, scene};
    MoviePresenter movies{runtime, vfs, jobs, audio.get()};
    DialoguePresenter presenter{runtime,
                                [&](const DialogueLine& line) { return say_box.glyph_count(line); },
                                kDefaultCharactersPerSecond,
                                [&](const DialogueLine& line, double speed, double elapsed) {
                                  return say_box.revealed(line, speed, elapsed);
                                }};
    Playback playback{runtime,
                      presenter,
                      persistent,
                      {.voice_playing = [&] { return audio->voice_playing(); },
                       .skip_wait = [&] { return waits.skip() || movies.skip(); }}};
    PlaybackIndicator indicator{ui};
    const Backlog backlog{runtime, audio.get(), config.backlog_length};
    BacklogScreen backlog_screen{ui, backlog, ui_renderer.text_measure()};
    PreferenceApplier preferences{runtime,
                                  {.presenter = &presenter,
                                   .playback = &playback,
                                   .audio = audio.get(),
                                   .platform = &platform}};

    SteadyClock clock;
    MainLoop loop{platform, clock, {.min_frame_time = kFrameTime}};
    ScreenActions actions{runtime,
                          ui,
                          saves,
                          {.playback = &playback,
                           .backlog = &backlog_screen,
                           .quit = [&] { loop.request_quit(); },
                           .on_warning = warn}};
    screens.set_action_handler(
        [&](const ScreenActivation& activation) { return actions.handle(activation); });
    ui_renderer.set_generated_images(
        [&](const std::string& name) { return actions.thumbnail(name); });
    static_cast<void>(actions.open("quick_menu", {}, false));

    const auto viewport = [&] {
      return compute_viewport(virtual_size, platform.window_metrics().pixels, ScaleMode::kFit);
    };
    const auto inside_canvas = [&](PointF window_point) {
      const PointF point = window_to_virtual(viewport(), window_point);
      return point.x >= 0 && point.y >= 0 && point.x < static_cast<float>(virtual_size.width) &&
             point.y < static_cast<float>(virtual_size.height);
    };

    // A click, key or button ends a wait for tweens first (skip mode will do the same), or a
    // movie that can be skipped.
    const auto advance = [&] {
      if (!waits.skip() && !movies.click()) {
        presenter.advance();
      }
    };
    MainLoopHooks hooks;
    hooks.on_event = [&](const InputEvent& event) {
      // L opens the backlog, and L or Esc close it, unless the player types into an input.
      if (!ui.wants_text_input() && backlog_screen.handle_event(event, viewport())) {
        return;
      }
      if (screens.handle_event(event, viewport())) {
        return;
      }
      if (is_rollback_event(event)) {
        playback.set_skip(SkipMode::kOff);
        static_cast<void>(runtime.roll_back());
        return;
      }
      if (const auto* key = std::get_if<KeyEvent>(&event)) {
        if (key->pressed && !key->repeat && is_advance_key(key->key)) {
          playback.set_skip(SkipMode::kOff);
          advance();
        } else if (key->pressed && !key->repeat && key->key == Key::kA) {
          playback.set_auto(!playback.automatic());
        } else if (key->pressed && !key->repeat && key->key == Key::kS) {
          playback.toggle_skip(preferences.current().skip_unseen ? SkipMode::kAll
                                                                 : SkipMode::kSeen);
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
      saves.poll();  // a script's save or load, or a load in the middle of a line
      presenter.update(seconds);
      preferences.update();
      if (!backlog_screen.is_open() && !ui.has_modal()) {  // auto and skip wait while the player
        playback.update(seconds);                          // reads or sets something
      }
      if (playback.skip() != SkipMode::kOff) {
        static_cast<void>(audio->stop("voice", 0));  // skip mode plays no voice
      }
      scene.update(seconds);
      audio->update(seconds);
      movies.update(seconds);
      waits.update();
      screens.sync();
      indicator.sync(playback.skip(), playback.automatic());
      persistent.flush();
    };
    hooks.draw = [&](double /*alpha*/) {
      if (presenter.ended()) {
        loop.request_quit();
        return;
      }
      const Viewport placed = viewport();
      const std::optional<DialogueLine>& line = presenter.line();
      scene.prepare(*renderer, jobs);
      movies.prepare(*renderer);
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
      scene.draw(*renderer, canvas);
      movies.draw(*renderer, virtual_size);
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
