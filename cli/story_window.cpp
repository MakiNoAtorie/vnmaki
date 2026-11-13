#include "cli/story_window.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
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
#include "engine/platform/input_map.hpp"
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
#include "engine/tools/profiler.hpp"
#if VNMAKI_DEV_TOOLS
#include "engine/tools/debugger.hpp"
#include "engine/tools/file_watcher.hpp"
#include "engine/tools/hot_reload.hpp"
#include "engine/tools/log_buffer.hpp"
#include "engine/tools/observer_chain.hpp"
#include "engine/tools/overlay.hpp"
#include "engine/tools/story_graph.hpp"
#endif
#include "engine/ui/backlog_screen.hpp"
#include "engine/ui/caption_presenter.hpp"
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

/// The dyslexia-friendly font (the `dyslexia_font` preference); a game without it has no such
/// option.
constexpr std::string_view kDyslexiaFontPath = "fonts/dyslexia.ttf";

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

int run_story_window(const Vfs& vfs, [[maybe_unused]] const std::filesystem::path& project_dir,
                     const ProjectConfig& config, std::optional<std::uint64_t> max_frames,
                     const std::optional<std::filesystem::path>& data_directory_override,
                     std::ostream& err, const std::optional<std::filesystem::path>& profile_csv,
                     [[maybe_unused]] DapChannel* dap) {
  const Size virtual_size{.width = config.resolution.width, .height = config.resolution.height};
  try {
    SteadyClock clock;
    // First, so that it sees the loads of the fonts. Dev builds always profile, for the overlay; a
    // release build only for --profile.
    Profiler profiler{clock,
                      {.enabled = VNMAKI_DEV_TOOLS != 0 || profile_csv.has_value(),
                       .max_frames = profile_csv ? 0 : Profiler::kGraphFrames}};
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
    const FontSet plain_fonts = fonts;
    std::optional<Font> dyslexia_font;
    if (vfs.exists(kDyslexiaFontPath)) {
      dyslexia_font.emplace(load_font(vfs, kDyslexiaFontPath));
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
                           .gallery = config.gallery,
                           .canvas_width = virtual_size.width,
                           .canvas_height = virtual_size.height}};
    ui.set_bindings(&runtime);
#if VNMAKI_DEV_TOOLS
    LogBuffer dev_log;        // hears `print` and the warnings
    TraceRecorder dev_trace;  // hears the labels entered, for the story graph
    ObserverChain dev_observers{&dev_log, &dev_trace};
    runtime.set_observer(&dev_observers);
#endif
    const auto warn = [&](std::string_view message) {
      err << "vnmaki: " << message << '\n';
#if VNMAKI_DEV_TOOLS
      dev_log.add(LogLevel::kWarning, message);
#endif
    };
    const std::filesystem::path data_directory =
        data_directory_override ? *data_directory_override
                                : user_data_directory(kUserDataOrganization, config.title);
    PersistentStore persistent{runtime, data_directory, warn};
    persistent.load();  // before the scripts, so that their defaults see the saved values
    runtime.load_scripts();
#if VNMAKI_DEV_TOOLS
    // `vnmaki debug`: the pump (below) draws and handles OS events while the story is stopped.
    std::function<bool()> debug_pump;
    std::unique_ptr<Debugger> debugger;
    if (dap != nullptr) {
      debugger = std::make_unique<Debugger>(runtime, *dap, project_dir,
                                            [&] { return debug_pump && debug_pump(); });
    }
#endif

    SdlPlatform platform{WindowConfig{.title = config.title, .size = kDefaultWindowSize}};
    ui.set_platform(&platform);  // text input is on only while an `input` node has the keys
    const std::unique_ptr<SdlGpuRenderer> renderer = SdlGpuRenderer::create_for_window(platform);
#if VNMAKI_DEV_TOOLS
    DevOverlay dev_overlay{*renderer, platform, runtime, dev_log};  // F12; dies before the renderer
    dev_overlay.set_inspect_targets(&scene, audio.get());
    dev_overlay.set_story(vfs, dev_trace);
#endif
    JobSystem jobs;
    const TextureId canvas = renderer->create_render_target(virtual_size, TextureFilter::kLinear);
    UiRenderer ui_renderer{*renderer, fonts, &vfs};
    ui.set_text_measure(ui_renderer.text_measure());
    SayBoxView say_box{*renderer, std::move(fonts), SayBoxStyle::for_resolution(virtual_size)};
    // The say box is the Lua screen `say` (`nvl` for an NVL line); if it fails the built-in box is
    // drawn.
    say_box.set_screen_source(
        [&](const DialogueLine& line) -> std::optional<UiNode> {
          ScreenNodeDef args =
              line.nvl ? make_nvl_args(*line.nvl, true)
                       : ScreenNodeDef{.kind = "args",
                                       .props = {{"speaker", line.speaker}, {"text", line.text}}};
          if (line.color && !line.nvl) {
            args.props.emplace_back("color", std::format("#{:06x}", *line.color));
          }
          ScreenNodeDef tree;
          if (!runtime.build_screen(line.nvl ? "nvl" : "say", args, tree).empty()) {
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
#if VNMAKI_DEV_TOOLS
    if (!debugger) {
      runtime.start("start");  // with a debugger it starts after configurationDone, below
    }
#else
    runtime.start("start");
#endif
    saves.poll();
    ScreenPresenter screens{runtime, ui};
    WaitPresenter waits{runtime, scene};
    MoviePresenter movies{runtime, vfs, jobs, audio.get()};
    scene.set_warning_handler(warn);  // a file that fails to load or play must not end the game
    audio->set_warning_handler(warn);
    movies.set_warning_handler(warn);
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
    CaptionPresenter captions{runtime, ui, warn};
    InputMap input_map;  // PreferenceApplier fills it from the preferences
    screens.set_input_map(&input_map);
    const Backlog backlog{runtime, audio.get(), config.backlog_length};
    BacklogScreen backlog_screen{ui, backlog, ui_renderer.text_measure()};
    backlog_screen.set_input_map(&input_map);
    const auto use_dyslexia_font = [&](bool on) {
      if (on && !dyslexia_font) {
        warn(std::string{kDyslexiaFontPath} + " is missing: the dyslexia-friendly font is off");
      }
      const FontSet chosen =
          on && dyslexia_font ? with_dyslexia_font(plain_fonts, *dyslexia_font) : plain_fonts;
      ui_renderer.set_fonts(chosen);
      say_box.set_fonts(chosen);
      ui.relayout();
    };
    PreferenceApplier preferences{runtime,
                                  {.presenter = &presenter,
                                   .playback = &playback,
                                   .audio = audio.get(),
                                   .platform = &platform,
                                   .ui = &ui,
                                   .say_box = &say_box,
                                   .controls = &input_map,
                                   .dyslexia_font = use_dyslexia_font}};

#if VNMAKI_DEV_TOOLS
    dev_overlay.set_profiler(&profiler);
#endif
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
#if VNMAKI_DEV_TOOLS
    // Hot reload: the watcher batches the changed files, and update() applies them.
    HotReload hot_reload{runtime, scene, jobs, ui, dev_log};
    hot_reload.set_on_scripts_reloaded(
        [&] { static_cast<void>(actions.open("quick_menu", {}, false)); });
    std::optional<FileWatcher> watcher;
    try {
      watcher.emplace(project_dir);
    } catch (const std::runtime_error& error) {
      warn(std::string{"hot reload is off: "} + error.what());
    }
#endif

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
#if VNMAKI_DEV_TOOLS
      if (dev_overlay.handle_event(event)) {
        return;
      }
#endif
      // The Controls screen waits for a key or button.
      if (actions.capture(event)) {
        return;
      }
      // L opens the backlog, and L or Esc close it, unless the player types into an input.
      if (!ui.wants_text_input() && backlog_screen.handle_event(event, viewport())) {
        return;
      }
      if (screens.handle_event(event, viewport())) {
        return;
      }
      if (is_rollback_event(event, input_map)) {
        playback.set_skip(SkipMode::kOff);
        static_cast<void>(runtime.roll_back());
        return;
      }
      if (input_map.matches(InputAction::kAdvance, event)) {
        playback.set_skip(SkipMode::kOff);
        advance();
      } else if (input_map.matches(InputAction::kAuto, event)) {
        playback.set_auto(!playback.automatic());
      } else if (input_map.matches(InputAction::kSkip, event)) {
        playback.toggle_skip(preferences.current().skip_unseen ? SkipMode::kAll : SkipMode::kSeen);
      } else if (input_map.matches(InputAction::kMenu, event)) {
        static_cast<void>(actions.open("preferences"));
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
      }
    };
    hooks.update = [&](Duration step) {
#if VNMAKI_DEV_TOOLS
      if (debugger && !debugger->poll()) {
        loop.request_quit();
      }
      if (watcher) {
        if (const std::vector<std::string> changed = watcher->poll(); !changed.empty()) {
          hot_reload.apply(changed);
        }
      }
#endif
      const double seconds = std::chrono::duration<double>(step).count();
      saves.poll();  // a script's save or load, or a load in the middle of a line
      presenter.update(seconds);
      preferences.update();
      captions.update(audio->take_captions(), seconds, preferences.current().captions);
      if (!backlog_screen.is_open() && !ui.has_modal()) {  // auto and skip wait while the player
        playback.update(seconds);                          // reads or sets something
      }
      if (playback.skip() != SkipMode::kOff) {
        static_cast<void>(audio->stop("voice", 0));  // skip mode plays no voice
      }
      scene.update(seconds);
      audio->update(seconds);
      movies.update(seconds);
      waits.update(seconds);
      screens.sync();
      indicator.sync(playback.skip(), playback.automatic());
      persistent.flush();
    };
    if (profiler.enabled()) {
      hooks.frame_end = [&](const FrameTiming& timing) { profiler.end_frame(timing); };
    }
    hooks.draw = [&](double /*alpha*/) {
#if VNMAKI_DEV_TOOLS
      if (debugger && !debugger->poll()) {  // also settles a goto before the story seems to fail
        loop.request_quit();
      }
#endif
      if (presenter.ended()) {
        loop.request_quit();
        return;
      }
      profiler.begin_prepare();
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
      profiler.end_prepare();
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
#if VNMAKI_DEV_TOOLS
      dev_overlay.draw(platform.window_metrics().pixels);
#endif
      profiler.set_gpu(renderer->end_frame(), renderer->texture_memory());
    };
#if VNMAKI_DEV_TOOLS
    if (debugger) {
      debug_pump = [&] {
        while (const std::optional<InputEvent> event = platform.poll_event()) {
          if (std::holds_alternative<QuitRequestedEvent>(*event)) {
            return false;
          }
        }
        hooks.draw(0.0);
        return true;
      };
      while (true) {
        if (!debugger->poll()) {
          return 0;
        }
        if (debugger->configured()) {
          break;
        }
        if (!debug_pump()) {
          return 0;
        }
      }
      runtime.start("start");
    }
#endif
    loop.run(hooks, max_frames);
    if (profile_csv) {
      if (const std::string problem = profiler.write_csv_files(*profile_csv); !problem.empty()) {
        err << "vnmaki: " << problem << '\n';
        return kExitFailure;
      }
    }

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
