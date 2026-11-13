// Preferences and the default screens (docs/SPEC.md sections 15 and 17) without a window: bind,
// the toggle, slider, bar and grid nodes, `prefs`, the engine's Lua screens and their actions.
// The tests click and press keys on the laid-out screens, as a player would. Text is sized by
// estimate_text_size() on a 1920 by 1080 canvas.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/audio/audio.hpp"
#include "engine/dialogue/dialogue_presenter.hpp"
#include "engine/dialogue/playback.hpp"
#include "engine/dialogue/say_box.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/platform.hpp"
#include "engine/platform/viewport.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/saves/persistent_store.hpp"
#include "engine/saves/saves.hpp"
#include "engine/ui/preferences.hpp"
#include "engine/ui/screen_actions.hpp"
#include "engine/ui/screen_presenter.hpp"
#include "engine/ui/ui_build.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/ui/ui_layout.hpp"
#include "engine/ui/ui_node.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::GamepadButton;
using vnmaki::Key;
using vnmaki::NodeKind;
using vnmaki::PointF;
using vnmaki::ScreenValue;
using vnmaki::UiNode;

constexpr vnmaki::Size kCanvas{.width = 1920, .height = 1080};
const vnmaki::Viewport kViewport =
    vnmaki::compute_viewport(kCanvas, kCanvas, vnmaki::ScaleMode::kFit);

constexpr double kHalf = 0.5;
constexpr double kSomeSpeed = 80;
constexpr double kSpeedStep = 5;  // the step of the text speed slider
constexpr std::int64_t kSliderMax = 10;
constexpr std::int64_t kSliderStart = 3;
constexpr std::size_t kColumns = 6;
constexpr std::size_t kSlotsShown = 12;
constexpr std::size_t kGlyphs = 20;
constexpr std::int64_t kCellWidth = 20;
constexpr std::int64_t kGridGap = 4;

PointF center_of(const UiNode& node) {
  return {.x = node.rect.x + (node.rect.width / 2), .y = node.rect.y + (node.rect.height / 2)};
}

// The first node under `root` (depth first) that `matches`.
UiNode* find_node(UiNode& root, const std::function<bool(const UiNode&)>& matches) {
  if (matches(root)) {
    return &root;
  }
  for (UiNode& child : root.children) {
    if (UiNode* found = find_node(child, matches)) {
      return found;
    }
  }
  return nullptr;
}

vnmaki::KeyEvent press(Key key) { return {.key = key, .pressed = true}; }

vnmaki::GamepadButtonEvent tap(GamepadButton button) { return {.button = button, .pressed = true}; }

// A directory under the system temp directory, empty at the start and removed at the end.
class TempDir {
 public:
  explicit TempDir(const std::string& name)
      : path_(std::filesystem::temp_directory_path() / ("vnmaki_screens_test_" + name)) {
    std::filesystem::remove_all(path_);
  }
  TempDir(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir& operator=(TempDir&&) = delete;
  ~TempDir() {
    try {
      std::filesystem::remove_all(path_);
    } catch (...) {
      std::fputs("TempDir: cannot remove the directory\n", stderr);  // not worth a failure
    }
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

// Everything a story window has, minus the window and the GPU.
struct Game {
  TempDir dir;
  vnmaki::Vfs vfs;
  vnmaki::UiHost host{kCanvas};
  vnmaki::ScriptRuntime runtime;
  vnmaki::PersistentStore persistent;
  vnmaki::Saves saves;
  vnmaki::DialoguePresenter dialogue;
  vnmaki::Playback playback;
  vnmaki::Audio audio;
  vnmaki::HeadlessPlatform platform;
  vnmaki::ScreenPresenter screens;
  vnmaki::ScreenActions actions;
  vnmaki::PreferenceApplier preferences;
  std::vector<std::string> warnings;

  Game(const std::string& name, const char* script)
      : dir(name),
        vfs(vnmaki::test::memory_vfs({{"scripts/main.lua", script}})),
        runtime(vfs,
                {.screens = &host, .canvas_width = kCanvas.width, .canvas_height = kCanvas.height}),
        persistent(runtime, dir.path(), [this](std::string_view m) { warnings.emplace_back(m); }),
        saves(start(), {.directory = dir.path()}),
        dialogue(runtime, [](const vnmaki::DialogueLine&) { return kGlyphs; }),
        playback(runtime, dialogue, persistent),
        audio(vfs),
        screens(runtime, host),
        actions(runtime, host, saves,
                {.playback = &playback,
                 .on_warning = [this](std::string_view m) { warnings.emplace_back(m); }}),
        preferences(runtime, {.presenter = &dialogue,
                              .playback = &playback,
                              .audio = &audio,
                              .platform = &platform}) {
    screens.set_action_handler(
        [this](const vnmaki::ScreenActivation& a) { return actions.handle(a); });
  }

  // Runs before the presenters are made: they need a started runtime.
  vnmaki::ScriptRuntime& start() {
    host.set_bindings(&runtime);
    persistent.load();
    runtime.load_scripts();
    runtime.start("start");
    return runtime;
  }

  bool send(const vnmaki::InputEvent& event) { return screens.handle_event(event, kViewport); }

  void click(PointF at) {
    send(vnmaki::PointerButtonEvent{
        .button = vnmaki::MouseButton::kLeft, .pressed = true, .position = at});
    send(vnmaki::PointerButtonEvent{
        .button = vnmaki::MouseButton::kLeft, .pressed = false, .position = at});
  }

  // The node of screen `screen` whose label starts with `label`.
  UiNode* node(std::string_view screen, std::string_view label) {
    UiNode* root = host.find(screen);
    return root == nullptr ? nullptr : find_node(*root, [&](const UiNode& n) {
      return n.label.starts_with(label) &&
             (n.role == vnmaki::NodeRole::kButton || n.role == vnmaki::NodeRole::kToggle ||
              n.role == vnmaki::NodeRole::kSlider);
    });
  }

  // Clicks the control of that label; fails the test if there is none.
  void click_on(std::string_view screen, std::string_view label) {
    UiNode* found = node(screen, label);
    REQUIRE_MESSAGE(found != nullptr, std::string{label});
    click(center_of(*found));
  }

  [[nodiscard]] std::string narration() const {
    const vnmaki::BlockingRequest* request = runtime.request();
    const auto* narrate = request ? std::get_if<vnmaki::NarrateRequest>(request) : nullptr;
    return narrate ? narrate->text : std::string{};
  }

  [[nodiscard]] double number(std::string_view table, std::string_view key) const {
    const ScreenValue value = runtime.binding(table, key);
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
      return static_cast<double>(*integer);
    }
    return std::get<double>(value);
  }

  [[nodiscard]] bool flag(std::string_view table, std::string_view key) const {
    const ScreenValue value = runtime.binding(table, key);
    const auto* held = std::get_if<bool>(&value);
    return held != nullptr && *held;
  }
};

constexpr const char* kStory = R"(
label("start", function()
  narrate("one")
  store.mark = "second"
  narrate("two")
  narrate("three")
end)
)";

constexpr const char* kPanelStory = R"(
screen("panel", function()
  return vbox {
    toggle { "Fast", bind = bind(store, "fast") },
    slider { bind = bind(store, "volume"), min = 0, max = 10, step = 1, width = 200,
             label = "Volume" },
    bar { bind = bind(store, "volume"), min = 0, max = 10, width = 200 },
  }
end)
label("start", function()
  store.fast = false
  store.volume = 3
  show_screen("panel")
  narrate("x")
end)
)";

}  // namespace

TEST_CASE("bind: prefs read as defaults until the player sets them, and persist when set") {
  Game game{"prefs_defaults", kStory};

  CHECK(game.number("prefs", "text_speed") == vnmaki::Preferences{}.text_speed);
  CHECK(vnmaki::read_preferences(game.runtime) == vnmaki::Preferences{});  // Lua and C++ agree
  CHECK(game.runtime.persistent_snapshot().tables.front().entries.empty());

  CHECK(game.runtime.set_binding("prefs", "text_speed", kSomeSpeed).empty());
  CHECK(game.number("prefs", "text_speed") == kSomeSpeed);
  CHECK(game.runtime.set_binding("prefs", "fullscreen", true).empty());
  CHECK(game.runtime.persistent_snapshot().tables.size() == 2);  // persistent and persistent.prefs

  // Setting nil takes the key away, and the default is back.
  CHECK(game.runtime.set_binding("prefs", "text_speed", ScreenValue{}).empty());
  CHECK(game.number("prefs", "text_speed") == vnmaki::Preferences{}.text_speed);
}

TEST_CASE("bind: a pref of the wrong type and an unknown table are refused") {
  Game game{"prefs_refused", kStory};

  CHECK(game.runtime.set_binding("prefs", "text_speed", true).find("number expected") !=
        std::string::npos);
  CHECK(game.runtime.set_binding("prefs", "fullscreen", 1.0).find("boolean expected") !=
        std::string::npos);
  CHECK(game.runtime.set_binding("nowhere", "x", true).find("unknown table") != std::string::npos);
  CHECK(game.number("prefs", "text_speed") == vnmaki::Preferences{}.text_speed);
}

TEST_CASE("bind: bind() takes only store, persistent or prefs") {
  const auto vfs = vnmaki::test::memory_vfs({{"scripts/main.lua", R"(
local ok, problem = pcall(bind, {}, "x")
label("start", function() narrate(tostring(ok) .. " " .. problem) end)
)"}});
  vnmaki::ScriptRuntime runtime{vfs};
  runtime.load_scripts();
  runtime.start("start");

  const auto* narrate = std::get_if<vnmaki::NarrateRequest>(runtime.request());
  REQUIRE(narrate != nullptr);
  CHECK(narrate->text.find("store, persistent or prefs expected") != std::string::npos);
}

TEST_CASE("ui nodes: a toggle, a slider and a bar follow store and write it back") {
  Game game{"nodes", kPanelStory};
  REQUIRE(game.host.is_shown("panel"));

  UiNode* toggle = game.node("panel", "Fast");
  UiNode* slider = game.node("panel", "Volume");
  REQUIRE((toggle != nullptr && slider != nullptr));
  CHECK_FALSE(toggle->checked);
  CHECK(slider->value == doctest::Approx(kSliderStart));

  game.click(center_of(*toggle));
  CHECK(game.flag("store", "fast"));
  CHECK(game.host.find("panel")->children[0].checked);

  // A click in the middle of the slider sets half the range, snapped to the step.
  game.click({.x = slider->rect.x + (slider->rect.width / 2),
              .y = slider->rect.y + (slider->rect.height / 2)});
  CHECK(game.number("store", "volume") == doctest::Approx(kSliderMax / 2));
  // The bar draws the same datum.
  const UiNode& bar = game.host.find("panel")->children[2];
  CHECK(bar.kind == NodeKind::kBar);
  CHECK(bar.value == doctest::Approx(kSliderMax / 2));

  // Dragging follows the pointer; the release ends it.
  const float left = slider->rect.x;
  const float middle = slider->rect.y + (slider->rect.height / 2);
  game.send(vnmaki::PointerButtonEvent{.pressed = true, .position = {.x = left, .y = middle}});
  game.send(vnmaki::PointerMoveEvent{.position = {.x = left + slider->rect.width, .y = middle}});
  CHECK(game.number("store", "volume") == doctest::Approx(kSliderMax));
  game.send(vnmaki::PointerButtonEvent{.pressed = false, .position = {.x = left, .y = middle}});
  game.send(vnmaki::PointerMoveEvent{.position = {.x = left, .y = middle}});
  CHECK(game.number("store", "volume") == doctest::Approx(kSliderMax));  // not dragging now
}

TEST_CASE("ui nodes: Tab gives the keys to a screen that is not modal, Esc takes them back") {
  Game game{"capture", kPanelStory};

  CHECK_FALSE(game.send(press(Key::kRight)));  // no screen has the keys: the story gets them
  CHECK(game.send(press(Key::kTab)));
  CHECK(game.node("panel", "Fast")->state.focused);
  CHECK(game.send(press(Key::kEnter)));  // flips the toggle
  CHECK(game.flag("store", "fast"));
  CHECK(game.send(press(Key::kDown)));  // to the slider
  CHECK(game.send(press(Key::kRight)));
  CHECK(game.number("store", "volume") == doctest::Approx(kSliderStart + 1));
  CHECK(game.send(press(Key::kLeft)));
  CHECK(game.send(press(Key::kLeft)));
  CHECK(game.number("store", "volume") == doctest::Approx(kSliderStart - 1));

  CHECK(game.send(press(Key::kEscape)));
  CHECK_FALSE(game.send(press(Key::kEnter)));  // the keys are back with the story
}

TEST_CASE("ui nodes: a gamepad also moves a slider and flips a toggle") {
  Game game{"pad", kPanelStory};

  CHECK(game.send(tap(GamepadButton::kWest)));  // gives the keys to the panel
  CHECK(game.send(tap(GamepadButton::kSouth)));
  CHECK(game.flag("store", "fast"));
  CHECK(game.send(tap(GamepadButton::kDpadDown)));
  CHECK(game.send(tap(GamepadButton::kDpadRight)));
  CHECK(game.number("store", "volume") == doctest::Approx(kSliderStart + 1));
  CHECK(game.send(tap(GamepadButton::kEast)));
}

TEST_CASE("ui nodes: a grid lays its children out in rows and a bad node is an error") {
  vnmaki::ScreenNodeDef cell{.kind = "box", .props = {{"width", kCellWidth}}};
  vnmaki::ScreenNodeDef grid{.kind = "grid",
                             .props = {{"columns", std::int64_t{2}}, {"gap", kGridGap}},
                             .children = {cell, cell, cell}};

  UiNode tree = vnmaki::build_ui_tree(grid);
  REQUIRE(tree.children.size() == 2);  // 3 cells, 2 to a row
  CHECK(tree.children[0].children.size() == 2);
  CHECK(tree.children[1].children.size() == 1);

  grid.props = {};
  CHECK_THROWS_AS(static_cast<void>(vnmaki::build_ui_tree(grid)), vnmaki::UiBuildError);
  vnmaki::ScreenNodeDef backwards{.kind = "slider",
                                  .props = {{"min", std::int64_t{2}}, {"max", std::int64_t{1}}}};
  CHECK_THROWS_AS(static_cast<void>(vnmaki::build_ui_tree(backwards)), vnmaki::UiBuildError);
  vnmaki::ScreenNodeDef stray{.kind = "bar", .props = {{"disabled", true}}};
  CHECK_THROWS_AS(static_cast<void>(vnmaki::build_ui_tree(stray)), vnmaki::UiBuildError);
}

TEST_CASE("preferences screen: a change is applied to the text, auto, audio and window") {
  Game game{"prefs_screen", kStory};
  game.actions.handle(
      {.screen = "quick_menu", .value = ScreenValue{std::string{"@open:preferences"}}});
  REQUIRE(game.host.is_shown("preferences"));
  const double auto_before = game.playback.auto_delay();

  UiNode* music = game.node("preferences", "Music volume");
  UiNode* speed = game.node("preferences", "Text speed");
  UiNode* delay = game.node("preferences", "Auto-advance delay");
  REQUIRE((music != nullptr && speed != nullptr && delay != nullptr));
  // The middle of each slider.
  const auto middle = [](const UiNode& slider) {
    return PointF{.x = slider.rect.x + (slider.rect.width / 2),
                  .y = slider.rect.y + (slider.rect.height / 2)};
  };
  game.click(middle(*music));
  game.click(middle(*speed));
  game.click(middle(*delay));
  game.click_on("preferences", "Fullscreen");
  game.click_on("preferences", "Window opacity");  // left edge is 0.3; the middle is 0.65
  game.preferences.update();

  CHECK(game.number("prefs", "volume_music") == doctest::Approx(kHalf));
  CHECK(game.audio.channel("music").preference == doctest::Approx(kHalf));
  CHECK(game.audio.channel("sound").preference == doctest::Approx(1));
  CHECK(game.platform.fullscreen());
  CHECK(game.platform.window_opacity() < 1);
  CHECK(game.dialogue.characters_per_second() ==
        doctest::Approx(game.number("prefs", "text_speed")));
  CHECK(game.dialogue.characters_per_second() != vnmaki::kDefaultCharactersPerSecond);
  CHECK(game.playback.auto_delay() - auto_before ==
        doctest::Approx(game.number("prefs", "auto_delay") - vnmaki::Preferences{}.auto_delay));

  // The preferences are in the persistent store, so they survive a restart.
  const vnmaki::StoreSnapshot saved = game.runtime.persistent_snapshot();
  CHECK(saved.tables.size() == 2);

  game.click_on("preferences", "Back");
  CHECK_FALSE(game.host.is_shown("preferences"));
}

TEST_CASE("preferences screen: the keyboard changes a slider and Esc closes the screen") {
  Game game{"prefs_keys", kStory};
  game.actions.handle(
      {.screen = "quick_menu", .value = ScreenValue{std::string{"@open:preferences"}}});
  REQUIRE(game.host.is_shown("preferences"));

  CHECK(game.node("preferences", "Text speed")->state.focused);  // the first control
  CHECK(game.send(press(Key::kRight)));
  CHECK(game.number("prefs", "text_speed") ==
        doctest::Approx(vnmaki::Preferences{}.text_speed + kSpeedStep));
  CHECK(game.send(press(Key::kDown)));
  CHECK(game.node("preferences", "Auto-advance delay")->state.focused);

  CHECK(game.send(press(Key::kEscape)));
  CHECK_FALSE(game.host.is_shown("preferences"));
}

TEST_CASE("save and load screens: a slot is saved through the grid and loaded again") {
  Game game{"save_load", kStory};
  game.actions.open("quick_menu", {}, false);
  REQUIRE(game.narration() == "one");
  game.runtime.advance();
  REQUIRE(game.narration() == "two");

  // Quick menu, Save, slot 3.
  game.click_on("quick_menu", "Save");
  REQUIRE(game.host.is_shown("save"));
  UiNode* grid = game.host.find("save");
  REQUIRE(grid->children.size() == 3);  // heading, grid, back
  CHECK(grid->children[1].children.size() == kSlotsShown / kColumns);
  CHECK(grid->children[1].children[0].children.size() == kColumns);
  const UiNode* slot = game.node("save", "Slot 3, empty");
  REQUIRE(slot != nullptr);
  CHECK(grid->children[1].children[0].children[2].label == slot->label);  // the third of row 1
  game.click(center_of(*slot));

  CHECK_FALSE(game.host.is_shown("save"));
  CHECK(game.saves.exists("3"));
  CHECK(game.warnings.empty());

  game.runtime.advance();
  REQUIRE(game.narration() == "three");

  // Load: the slot shows what was saved, with its thumbnail; empty slots cannot be chosen.
  game.click_on("quick_menu", "Load");
  REQUIRE(game.host.is_shown("load"));
  CHECK(game.node("load", "Slot 3, two") != nullptr);
  CHECK(game.node("load", "Slot 4, empty")->state.disabled);
  const UiNode* picture = find_node(*game.host.find("load"),
                                    [](const UiNode& n) { return n.kind == NodeKind::kImage; });
  REQUIRE(picture != nullptr);
  const std::optional<vnmaki::Image> thumbnail = game.actions.thumbnail(picture->text);
  REQUIRE(thumbnail.has_value());
  CHECK(thumbnail->size.width > 0);

  game.click_on("load", "Slot 3");
  CHECK_FALSE(game.host.is_shown("load"));
  CHECK(game.narration() == "two");
  CHECK(game.warnings.empty());
}

TEST_CASE("save and load screens: keyboard and gamepad choose the slot; saving over asks first") {
  Game game{"save_pad", kStory};
  game.actions.open("quick_menu", {}, false);
  game.runtime.advance();

  // The keyboard: Tab to the quick menu, Enter on its first button (Save), then pick slot 1.
  CHECK(game.send(press(Key::kTab)));
  CHECK(game.send(press(Key::kEnter)));
  REQUIRE(game.host.is_shown("save"));
  CHECK(game.node("save", "Slot 1")->state.focused);
  CHECK(game.send(press(Key::kEnter)));
  CHECK(game.saves.exists("1"));

  // Saving to the slot again asks. No keeps the old save and the grid, Yes replaces it.
  game.click_on("quick_menu", "Save");
  game.click_on("save", "Slot 1");
  REQUIRE(game.host.is_shown("confirm"));
  CHECK(game.send(press(Key::kEscape)));  // Esc answers No
  CHECK_FALSE(game.host.is_shown("confirm"));
  CHECK(game.host.is_shown("save"));
  game.runtime.advance();
  game.click_on("save", "Slot 1");
  game.click_on("confirm", "Yes");
  CHECK_FALSE(game.host.is_shown("confirm"));
  CHECK_FALSE(game.host.is_shown("save"));

  // The gamepad: Load with the d-pad and the south button. Only slot 1 is in use.
  game.click_on("quick_menu", "Load");
  CHECK(game.node("load", "Slot 1")->state.focused);
  CHECK(game.send(tap(GamepadButton::kSouth)));
  CHECK(game.narration() == "three");  // slot 1 was saved the second time, at the third line
  CHECK(game.warnings.empty());
}

TEST_CASE("quick menu: auto, skip and quit act on the story window") {
  Game game{"quick", kStory};
  game.actions.open("quick_menu", {}, false);

  game.click_on("quick_menu", "Auto");
  CHECK(game.playback.automatic());
  game.click_on("quick_menu", "Auto");
  CHECK_FALSE(game.playback.automatic());
  game.click_on("quick_menu", "Skip");
  CHECK(game.playback.skip() == vnmaki::SkipMode::kSeen);
  game.click_on("quick_menu", "Skip");
  CHECK(game.playback.skip() == vnmaki::SkipMode::kOff);
  CHECK(game.runtime.set_binding("prefs", "skip_unseen", true).empty());
  game.click_on("quick_menu", "Skip");
  CHECK(game.playback.skip() == vnmaki::SkipMode::kAll);

  // Quit asks first; No leaves the story running.
  game.actions.handle({.screen = "title", .value = ScreenValue{std::string{"@quit"}}});
  REQUIRE(game.host.is_shown("confirm"));
  game.click_on("confirm", "No");
  CHECK_FALSE(game.host.is_shown("confirm"));
}

TEST_CASE("title screen: call_screen returns Start, and Load opens over it") {
  Game game{"title", R"(
label("start", function()
  local choice = call_screen("title", { title = "My Game" })
  narrate("chose " .. choice)
end)
)"};
  REQUIRE(game.host.is_shown("title"));
  CHECK(game.node("title", "Start") != nullptr);

  game.click_on("title", "Load");
  REQUIRE(game.host.is_shown("load"));
  CHECK(game.host.is_shown("title"));  // still waiting under the load screen
  game.click_on("load", "Back");
  CHECK_FALSE(game.host.is_shown("load"));

  game.click_on("title", "Start");
  CHECK_FALSE(game.host.is_shown("title"));
  CHECK(game.narration() == "chose start");
}

TEST_CASE("default screens: the say screen has the geometry of the built-in say box") {
  Game game{"say", kStory};
  const vnmaki::DialogueLine line{.speaker = "Eileen", .text = "Hello"};
  vnmaki::ScreenNodeDef args{.kind = "args",
                             .props = {{"speaker", line.speaker}, {"text", line.text}}};
  vnmaki::ScreenNodeDef tree;
  REQUIRE(game.runtime.build_screen("say", args, tree).empty());

  UiNode screen = vnmaki::build_ui_tree(tree);
  vnmaki::layout_tree(screen, kCanvas, vnmaki::estimate_text_size);
  const vnmaki::SayBoxStyle style = vnmaki::SayBoxStyle::for_resolution(kCanvas);
  const UiNode* reveal = find_node(screen, [](const UiNode& n) { return n.reveal; });
  REQUIRE(reveal != nullptr);
  CHECK(reveal->markup);
  CHECK(reveal->rect.x == style.box.x + style.text_offset.x);
  CHECK(reveal->rect.y == style.box.y + style.text_offset.y);
  CHECK(reveal->style.width == style.wrap_width);
  CHECK(reveal->style.text_size == style.text_size);
  const UiNode* name = find_node(screen, [](const UiNode& n) { return n.text == "Eileen"; });
  REQUIRE(name != nullptr);
  CHECK(name->rect.x == style.box.x + style.name_offset.x);
  CHECK(name->style.text_size == style.name_size);
}

TEST_CASE("default screens: a game replaces a screen by defining one with the same name") {
  Game game{"replace", R"(
screen("choice", function(args)
  local buttons = {}
  for i, option in ipairs(args) do
    buttons[i] = button { "> " .. option.text, returns = i }
  end
  return vbox { table.unpack(buttons) }
end)
label("start", function()
  narrate("picked " .. menu { "a", "b" })
end)
)"};

  REQUIRE(game.host.is_shown(vnmaki::kMenuScreenName));
  CHECK(game.node(vnmaki::kMenuScreenName, "> b") != nullptr);
  game.click_on(vnmaki::kMenuScreenName, "> b");
  CHECK(game.narration() == "picked 2");
}

TEST_CASE("default screens: a screen that fails falls back to the built-in menu") {
  Game game{"broken_choice", R"(
screen("choice", function(args) error("my choice screen is broken") end)
label("start", function()
  narrate("picked " .. menu { "a", "b" })
end)
)"};

  REQUIRE(game.host.is_shown(vnmaki::kMenuScreenName));
  game.click_on(vnmaki::kMenuScreenName, "a");
  CHECK(game.narration() == "picked 1");
}
