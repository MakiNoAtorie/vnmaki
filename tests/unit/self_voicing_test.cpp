// Self-voicing (docs/SPEC.md section 17) without a window or a voice: a recording backend gets the
// exact strings of scripted sessions. Time stands still; a frame is presenter, sync, voicing.

#include "engine/ui/self_voicing.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "engine/dialogue/dialogue_presenter.hpp"
#include "engine/dialogue/playback.hpp"
#include "engine/l10n/extract.hpp"
#include "engine/l10n/po.hpp"
#include "engine/l10n/translator.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/input_map.hpp"
#include "engine/platform/tts.hpp"
#include "engine/platform/viewport.hpp"
#include "engine/runtime/project_config.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/saves/persistent_store.hpp"
#include "engine/ui/preferences.hpp"
#include "engine/ui/screen_presenter.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"
#include "tests/support/recording_tts.hpp"
#include "tests/support/temp_dir.hpp"

namespace {

using vnmaki::Key;
using vnmaki::SkipMode;

constexpr vnmaki::Size kCanvas{.width = 1920, .height = 1080};
const vnmaki::Viewport kViewport =
    vnmaki::compute_viewport(kCanvas, kCanvas, vnmaki::ScaleMode::kFit);

constexpr std::size_t kGlyphs = 20;
constexpr double kReveal = 100;  // seconds: the typewriter has finished

// The Japanese of the session below: its first line and the first option of its menu.
std::string ja_po() {
  vnmaki::PoFile file;
  file.entries.push_back(vnmaki::po_header("ja"));
  const auto add = [&](std::string speaker, std::string text, std::string translation) {
    vnmaki::PoEntry entry;
    entry.context = vnmaki::translation_key("start", speaker, text);
    entry.id = std::move(text);
    entry.str = std::move(translation);
    file.entries.push_back(std::move(entry));
  };
  add("Alice", "Hello {color=#f00}there{/color}.", "Konnichiwa {color=#f00}koko{/color}.");
  add("", "Stay", "Todomaru");
  return vnmaki::write_po(file);
}

// A story with a presenter, a screen presenter and self-voicing, as the story window has them.
struct Game {
  explicit Game(const char* script)
      : directory(vnmaki::test::unique_temp_path("vnmaki_voicing_test_", "game")),
        vfs(vnmaki::test::memory_vfs(
            {{"scripts/main.lua", script}, {"translations/ja.po", ja_po()}})),
        runtime(vfs,
                {.screens = &host, .canvas_width = kCanvas.width, .canvas_height = kCanvas.height}),
        persistent(runtime, directory, [](std::string_view) {}) {
    host.set_bindings(&runtime);
    persistent.load();
    runtime.load_scripts();
    runtime.start("start");
    presenter = std::make_unique<vnmaki::DialoguePresenter>(
        runtime, [](const vnmaki::DialogueLine&) { return kGlyphs; });
    playback = std::make_unique<vnmaki::Playback>(runtime, *presenter, persistent);
    screens = std::make_unique<vnmaki::ScreenPresenter>(runtime, host);
    voicing = std::make_unique<vnmaki::SelfVoicing>(tts, runtime, *presenter, *playback, host);
  }
  Game(const Game&) = delete;
  Game& operator=(const Game&) = delete;
  Game(Game&&) = delete;
  Game& operator=(Game&&) = delete;
  ~Game() { std::filesystem::remove_all(directory); }

  void frame() {
    presenter->update(kReveal);
    screens->sync();
    voicing->update();
  }

  void press(Key key, bool shift = false) {
    const vnmaki::InputEvent event =
        vnmaki::KeyEvent{.key = key, .pressed = true, .mods = {.shift = shift}};
    if (!screens->handle_event(event, kViewport)) {
      presenter->advance();
    }
    frame();
  }

  std::filesystem::path directory;
  vnmaki::UiHost host{kCanvas};
  vnmaki::Vfs vfs;
  vnmaki::ScriptRuntime runtime;
  vnmaki::PersistentStore persistent;
  vnmaki::test::RecordingTts tts;
  std::unique_ptr<vnmaki::DialoguePresenter> presenter;
  std::unique_ptr<vnmaki::Playback> playback;
  std::unique_ptr<vnmaki::ScreenPresenter> screens;
  std::unique_ptr<vnmaki::SelfVoicing> voicing;
};

constexpr const char* kSession = R"(
screen("panel", function()
  return vbox {
    modal = true,
    button { "Start", returns = "go" },
    toggle { "Fast", bind = bind(store, "fast") },
    slider { bind = bind(store, "volume"), min = 0, max = 10, step = 1, width = 200,
             label = "Volume" },
  }
end)
label("start", function()
  store.fast = false
  store.volume = 3
  say("Alice", "Hello {color=#f00}there{/color}.")
  narrate("The door opens.")
  local pick = menu { "Stay", "Go" }
  narrate("You chose " .. pick)
  call_screen("panel")
  narrate("The end.")
end)
)";

using Strings = std::vector<std::string>;

}  // namespace

TEST_CASE("self-voicing: a scripted session is spoken line by line") {
  Game game{kSession};
  game.voicing->set_enabled(true);
  game.frame();
  game.press(Key::kSpace);  // advances the line
  game.press(Key::kSpace);  // advances the narration to the menu
  game.press(Key::kDown);   // moves the focus to the second option
  game.press(Key::kEnter);  // chooses it
  game.press(Key::kSpace);  // advances the narration to the panel
  game.press(Key::kTab);    // Fast
  game.press(Key::kEnter);  // flips it
  game.press(Key::kTab);    // Volume
  game.press(Key::kRight);  // one step up

  CHECK(game.tts.spoken == Strings{
                               "Alice: Hello there.",
                               "The door opens.",
                               "Choose. Stay. Go",
                               "Go, button",
                               "You chose 2",
                               "Start, button",
                               "Fast, toggle, off",
                               "Fast, toggle, on",
                               "Volume, slider, 30 percent",
                               "Volume, slider, 40 percent",
                           });
}

TEST_CASE("self-voicing: nothing is spoken while it is off, and it speaks the line on again") {
  Game game{kSession};
  game.frame();
  game.press(Key::kSpace);
  CHECK(game.tts.spoken.empty());

  game.voicing->set_enabled(true);
  game.frame();
  CHECK(game.tts.spoken == Strings{"The door opens."});
  game.frame();  // the same line is not said twice
  CHECK(game.tts.spoken.size() == 1);

  game.voicing->set_enabled(false);
  game.press(Key::kSpace);
  CHECK(game.tts.spoken == Strings{"The door opens.", "Self-voicing off"});
}

TEST_CASE("self-voicing: skip mode speaks nothing and stops the speech") {
  Game game{kSession};
  game.voicing->set_enabled(true);
  game.frame();
  REQUIRE(game.tts.spoken.size() == 1);
  CHECK(game.tts.stops == 0);

  game.playback->set_skip(SkipMode::kAll);
  game.frame();
  CHECK(game.tts.stops == 1);
  game.presenter->advance();
  game.frame();
  game.presenter->advance();  // the menu
  game.frame();
  CHECK(game.tts.spoken.size() == 1);
  CHECK(game.tts.stops == 1);

  // Out of skip mode, the next change is spoken again; the menu that passed in silence is not.
  game.playback->set_skip(SkipMode::kOff);
  game.frame();
  CHECK(game.tts.spoken.size() == 1);
  game.press(Key::kDown);
  CHECK(game.tts.spoken == Strings{"Alice: Hello there.", "Go, button"});
}

TEST_CASE("self-voicing: the preference turns it on and off") {
  Game game{kSession};
  vnmaki::PreferenceApplier applier{game.runtime, {.self_voicing = game.voicing.get()}};
  applier.update();
  CHECK_FALSE(game.voicing->enabled());

  CHECK(game.runtime.set_binding("prefs", "self_voicing", true).empty());
  applier.update();
  CHECK(game.voicing->enabled());
  CHECK(vnmaki::read_preferences(game.runtime).self_voicing);
  game.frame();
  CHECK(game.tts.spoken == Strings{"Alice: Hello there."});

  CHECK(game.runtime.set_binding("prefs", "self_voicing", false).empty());
  applier.update();
  CHECK_FALSE(game.voicing->enabled());
  CHECK(game.tts.spoken == Strings{"Alice: Hello there.", "Self-voicing off"});
}

TEST_CASE("self-voicing: the preferences screen has a toggle for it") {
  Game game{R"(
label("start", function()
  call_screen("preferences")
end)
)"};
  game.voicing->set_enabled(true);
  // The first focus is the first control; Shift+Tab goes back over Back and Controls.
  game.frame();
  game.press(Key::kTab, true);
  game.press(Key::kTab, true);
  game.press(Key::kTab, true);
  CHECK(game.tts.spoken.back() == "Self-voicing, toggle, off");
  game.press(Key::kEnter);
  CHECK(game.tts.spoken.back() == "Self-voicing, toggle, on");
}

TEST_CASE("self-voicing: V is the default key of the input action, and a preference remaps it") {
  const vnmaki::InputEvent v = vnmaki::KeyEvent{.key = Key::kV, .pressed = true};
  const vnmaki::InputEvent f2 = vnmaki::KeyEvent{.key = Key::kF2, .pressed = true};
  CHECK(vnmaki::InputMap{}.matches(vnmaki::InputAction::kSelfVoicing, v));

  Game game{kSession};
  CHECK(game.runtime.set_binding("prefs", "input_self_voicing", "key:f2").empty());
  const vnmaki::InputMap controls = vnmaki::read_preferences(game.runtime).controls;
  CHECK(controls.matches(vnmaki::InputAction::kSelfVoicing, f2));
  CHECK_FALSE(controls.matches(vnmaki::InputAction::kSelfVoicing, v));
}

TEST_CASE("self-voicing: a language is spoken in its own text") {
  Game game{kSession};
  const vnmaki::ProjectConfig config{.title = "T", .languages = {"en", "ja"}};
  vnmaki::Translator translator{game.vfs, config};
  game.presenter->set_translator(&translator);
  game.screens->set_translator(&translator);
  game.voicing->set_translator(&translator);
  REQUIRE(translator.set_language("ja").empty());
  game.voicing->set_enabled(true);
  game.frame();
  game.press(Key::kSpace);  // advances the line
  game.press(Key::kSpace);  // advances the narration (no entry: the source text) to the menu

  CHECK(game.tts.spoken == Strings{
                               "Alice: Konnichiwa koko.", "The door opens.",
                               "Choose. Todomaru. Go",  // the focus follows silently
                           });
}
