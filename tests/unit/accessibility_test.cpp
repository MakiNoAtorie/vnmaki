// Accessibility options (docs/SPEC.md section 17, task 42) without a window: text scale, high
// contrast, the dyslexia-friendly font, captions, reduced motion, and remappable controls. The
// remap tests press the keys and buttons of the Controls screen as a player would.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/audio/audio.hpp"
#include "engine/dialogue/dialogue_presenter.hpp"
#include "engine/dialogue/playback.hpp"
#include "engine/dialogue/rollback_input.hpp"
#include "engine/dialogue/say_box.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/input_map.hpp"
#include "engine/platform/platform.hpp"
#include "engine/platform/viewport.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/saves/persistent_store.hpp"
#include "engine/saves/saves.hpp"
#include "engine/scene/scene.hpp"
#include "engine/text/text.hpp"
#include "engine/ui/caption_presenter.hpp"
#include "engine/ui/preferences.hpp"
#include "engine/ui/screen_actions.hpp"
#include "engine/ui/screen_presenter.hpp"
#include "engine/ui/ui_build.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/ui/ui_layout.hpp"
#include "engine/ui/ui_node.hpp"
#include "engine/ui/ui_renderer.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/audio_fixtures.hpp"
#include "tests/support/memory_vfs.hpp"
#include "tests/support/recording_renderer.hpp"
#include "tests/support/temp_dir.hpp"

namespace {

namespace fs = std::filesystem;

using vnmaki::Binding;
using vnmaki::GamepadButton;
using vnmaki::InputAction;
using vnmaki::InputMap;
using vnmaki::Key;
using vnmaki::NodeKind;
using vnmaki::PointF;
using vnmaki::ScreenValue;
using vnmaki::UiNode;

constexpr vnmaki::Size kCanvas{.width = 1920, .height = 1080};
const vnmaki::Viewport kViewport =
    vnmaki::compute_viewport(kCanvas, kCanvas, vnmaki::ScaleMode::kFit);

constexpr double kDoubled = 2;
constexpr double kScale = 1.5;
constexpr double kTooSmall = 0.1;
constexpr double kTooBig = 5;
constexpr float kBigScale = 2;
constexpr float kTextSize = 20;
constexpr float kWrapMargin = 1.2F;
constexpr float kFlatWidth = 100000;
constexpr double kSecond = 1;
constexpr double kGlyphRounding = 0.05;  // a glyph bitmap is a whole number of pixels
constexpr double kTenth = 0.1;
constexpr double kQuarter = 0.25;
constexpr double kMoment = 0.3;
constexpr double kFive = 5;
constexpr double kSoundSeconds = 0.25;  // the length of tone.wav
constexpr std::size_t kThreeCaptions = 3;
constexpr std::size_t kLineGlyphs = 20;

vnmaki::KeyEvent press(Key key) { return {.key = key, .pressed = true}; }
vnmaki::GamepadButtonEvent tap(GamepadButton button) { return {.button = button, .pressed = true}; }

PointF center_of(const UiNode& node) {
  return {.x = node.rect.x + (node.rect.width / 2), .y = node.rect.y + (node.rect.height / 2)};
}

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

std::string read_file(const fs::path& path) {
  std::ifstream file{path, std::ios::binary};
  REQUIRE_MESSAGE(file.good(), path.string());
  return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

fs::path template_dir() { return fs::path{VNMAKI_TEMPLATES_DIR} / "default"; }

class TempDir {
 public:
  explicit TempDir(const std::string& name)
      : path_(vnmaki::test::unique_temp_path("vnmaki_a11y_test_", name)) {
    fs::remove_all(path_);
  }
  TempDir(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir& operator=(TempDir&&) = delete;
  ~TempDir() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }

  [[nodiscard]] const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

constexpr const char* kStory = R"(
label("start", function()
  narrate("one")
  narrate("two")
end)
)";

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
  InputMap controls;
  std::vector<std::string> warnings;
  std::vector<bool> dyslexia_calls;

  Game(const std::string& name, const std::string& script, bool with_contrast = true)
      : dir(name),
        vfs(files(script, with_contrast)),
        runtime(vfs,
                {.screens = &host, .canvas_width = kCanvas.width, .canvas_height = kCanvas.height}),
        persistent(runtime, dir.path(), [this](std::string_view m) { warnings.emplace_back(m); }),
        saves(start(), {.directory = dir.path()}),
        dialogue(runtime, [](const vnmaki::DialogueLine&) { return kLineGlyphs; }),
        playback(runtime, dialogue, persistent),
        audio(vfs),
        screens(runtime, host),
        actions(runtime, host, saves,
                {.playback = &playback,
                 .on_warning = [this](std::string_view m) { warnings.emplace_back(m); }}),
        preferences(runtime, {.presenter = &dialogue,
                              .playback = &playback,
                              .audio = &audio,
                              .platform = &platform,
                              .ui = &host,
                              .controls = &controls,
                              .dyslexia_font = [this](bool on) { dyslexia_calls.push_back(on); }}) {
    screens.set_input_map(&controls);
    screens.set_action_handler(
        [this](const vnmaki::ScreenActivation& a) { return actions.handle(a); });
  }

  static vnmaki::Vfs files(const std::string& script, bool with_contrast) {
    std::map<std::string, std::string, std::less<>> all{{"scripts/main.lua", script}};
    if (with_contrast) {
      all["styles/high_contrast.lua"] = read_file(template_dir() / "styles/high_contrast.lua");
    }
    return vnmaki::test::memory_vfs(std::move(all));
  }

  vnmaki::ScriptRuntime& start() {
    host.set_bindings(&runtime);
    persistent.load();
    runtime.load_scripts();
    runtime.start("start");
    return runtime;
  }

  void click(PointF at) {
    static_cast<void>(screens.handle_event(
        vnmaki::PointerButtonEvent{
            .button = vnmaki::MouseButton::kLeft, .pressed = true, .position = at},
        kViewport));
    static_cast<void>(screens.handle_event(
        vnmaki::PointerButtonEvent{
            .button = vnmaki::MouseButton::kLeft, .pressed = false, .position = at},
        kViewport));
  }

  UiNode* node(std::string_view screen, std::string_view label) {
    UiNode* root = host.find(screen);
    return root == nullptr ? nullptr : find_node(*root, [&](const UiNode& n) {
      return n.label.starts_with(label) &&
             (n.role == vnmaki::NodeRole::kButton || n.role == vnmaki::NodeRole::kToggle ||
              n.role == vnmaki::NodeRole::kSlider);
    });
  }

  void click_on(std::string_view screen, std::string_view label) {
    UiNode* found = node(screen, label);
    REQUIRE_MESSAGE(found != nullptr, std::string{label});
    click(center_of(*found));
  }

  [[nodiscard]] double number(std::string_view key) const {
    const ScreenValue value = runtime.binding("prefs", key);
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
      return static_cast<double>(*integer);
    }
    return std::get<double>(value);
  }

  [[nodiscard]] bool flag(std::string_view key) const {
    const ScreenValue value = runtime.binding("prefs", key);
    const auto* held = std::get_if<bool>(&value);
    return held != nullptr && *held;
  }

  // The Lua screen `name` for `args`, as the engine builds it.
  UiNode build(std::string_view name, const vnmaki::ScreenNodeDef& args = {}) {
    vnmaki::ScreenNodeDef tree;
    const std::string problem = runtime.build_screen(name, args, tree);
    REQUIRE_MESSAGE(problem.empty(), problem);
    return vnmaki::build_ui_tree(tree);
  }

  void open(std::string_view action) {
    actions.handle({.screen = "quick_menu", .value = ScreenValue{std::string{action}}});
  }
};

}  // namespace

// --- Controls ----------------------------------------------------------------------------------

TEST_CASE("input map: a binding round trips through its preference text, key by key") {
  const InputMap defaults;
  for (const InputAction action : vnmaki::kInputActions) {
    const Binding& binding = defaults.binding(action);
    CHECK(vnmaki::parse_binding(vnmaki::format_binding(binding)) == binding);
  }
  for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(Key::kRightGui); ++i) {
    const Binding binding{.keys = {static_cast<Key>(i)}, .buttons = {}};
    CHECK(vnmaki::parse_binding(vnmaki::format_binding(binding)) == binding);
  }
  for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(GamepadButton::kDpadRight); ++i) {
    const Binding binding{.keys = {}, .buttons = {static_cast<GamepadButton>(i)}};
    CHECK(vnmaki::parse_binding(vnmaki::format_binding(binding)) == binding);
  }
  CHECK(vnmaki::format_binding(defaults.binding(InputAction::kAdvance)) ==
        "key:enter key:keypad_enter key:space pad:south");
  CHECK(vnmaki::describe_binding(defaults.binding(InputAction::kRollback)) ==
        "Page up / Left shoulder");
  CHECK(vnmaki::describe_binding({}) == "None");

  CHECK_FALSE(vnmaki::parse_binding("key:nonsense"));
  CHECK_FALSE(vnmaki::parse_binding("mouse:left"));
  CHECK(vnmaki::parse_binding("")->keys.empty());
  CHECK(vnmaki::action_from_id("skip") == InputAction::kSkip);
  CHECK_FALSE(vnmaki::action_from_id("jump"));
}

TEST_CASE("input map: a press matches, a release and a repeat do not, and binding takes over") {
  InputMap map;
  CHECK(map.matches(InputAction::kSkip, press(Key::kS)));
  CHECK_FALSE(map.matches(InputAction::kSkip, vnmaki::KeyEvent{.key = Key::kS, .pressed = false}));
  CHECK_FALSE(map.matches(InputAction::kSkip,
                          vnmaki::KeyEvent{.key = Key::kS, .pressed = true, .repeat = true}));
  CHECK(map.matches(InputAction::kSkip, tap(GamepadButton::kRightShoulder)));
  CHECK_FALSE(map.matches(InputAction::kSkip, vnmaki::WheelEvent{.dy = 1}));

  map.bind_key(InputAction::kSkip, Key::kA);  // A was the key of auto-advance
  CHECK(map.matches(InputAction::kSkip, press(Key::kA)));
  CHECK_FALSE(map.matches(InputAction::kSkip, press(Key::kS)));
  CHECK_FALSE(map.matches(InputAction::kAuto, press(Key::kA)));
  map.reset();
  CHECK(map == InputMap{});
}

TEST_CASE("controls: the defaults of the preferences in Lua are those of the InputMap") {
  Game game{"controls_defaults", kStory};
  CHECK(vnmaki::read_preferences(game.runtime) == vnmaki::Preferences{});
}

TEST_CASE("controls: the Controls screen remaps with injected keys and buttons, and it persists") {
  Game game{"remap", kStory};
  game.open("@open:preferences");
  game.click_on("preferences", "Controls");
  REQUIRE(game.host.is_shown("controls"));
  REQUIRE_FALSE(game.actions.capturing());

  // A key changes the keys of its action; the gamepad buttons stay.
  game.click_on("controls", "Skip");
  CHECK(game.actions.capturing());
  CHECK(game.actions.capture(press(Key::kTab)));  // the UI keeps Tab: the wait goes on
  CHECK(game.actions.capturing());
  CHECK(game.actions.capture(press(Key::kF2)));
  CHECK_FALSE(game.actions.capturing());
  game.preferences.update();
  CHECK(game.controls.matches(InputAction::kSkip, press(Key::kF2)));
  CHECK_FALSE(game.controls.matches(InputAction::kSkip, press(Key::kS)));
  CHECK(game.controls.matches(InputAction::kSkip, tap(GamepadButton::kRightShoulder)));
  const UiNode* skip = game.node("controls", "Skip");
  REQUIRE(skip != nullptr);
  CHECK(skip->text.find("F2") != std::string::npos);

  // A key that another action had goes to the new action.
  game.click_on("controls", "Auto-advance");
  CHECK(game.actions.capture(press(Key::kF2)));
  game.preferences.update();
  CHECK(game.controls.matches(InputAction::kAuto, press(Key::kF2)));
  CHECK_FALSE(game.controls.matches(InputAction::kSkip, press(Key::kF2)));

  // A gamepad button changes the buttons, and Esc cancels.
  game.click_on("controls", "Roll back");
  CHECK(game.actions.capture(tap(GamepadButton::kNorth)));
  game.preferences.update();
  CHECK(vnmaki::is_rollback_event(tap(GamepadButton::kNorth), game.controls));
  CHECK_FALSE(vnmaki::is_rollback_event(tap(GamepadButton::kLeftShoulder), game.controls));
  CHECK_FALSE(game.controls.matches(InputAction::kBacklog, tap(GamepadButton::kNorth)));
  CHECK(vnmaki::is_rollback_event(press(Key::kPageUp), game.controls));
  game.click_on("controls", "Backlog");
  CHECK(game.actions.capture(press(Key::kEscape)));
  CHECK_FALSE(game.actions.capturing());
  CHECK(game.host.is_shown("controls"));
  CHECK_FALSE(game.actions.capture(press(Key::kF3)));  // not waiting: the event is not used

  // The map is in `persistent`, so a restart has it.
  Game restarted{"remap_restarted", kStory};
  CHECK(restarted.runtime.restore_persistent(game.runtime.persistent_snapshot()).empty());
  restarted.preferences.update();
  CHECK(restarted.controls == game.controls);

  game.click_on("controls", "Reset to defaults");
  game.preferences.update();
  CHECK(game.controls == InputMap{});
}

TEST_CASE("controls: the rollback input of a menu follows the map") {
  Game game{"remap_menu", R"(
label("start", function()
  narrate("picked " .. menu { "a", "b" })
end)
)"};
  REQUIRE(game.host.is_shown(vnmaki::kMenuScreenName));
  InputMap map;
  map.bind_key(InputAction::kRollback, Key::kF4);
  game.screens.set_input_map(&map);
  // The menu lets its own rollback input through, and eats the old one.
  CHECK_FALSE(game.screens.handle_event(press(Key::kF4), kViewport));
  CHECK(game.screens.handle_event(press(Key::kPageUp), kViewport));
}

// --- Text scale --------------------------------------------------------------------------------

TEST_CASE("text scale: the preference is clamped and the host scales every screen") {
  Game game{"scale", kStory};
  CHECK(vnmaki::read_preferences(game.runtime).text_scale == 1);

  UiNode text = vnmaki::make_node(NodeKind::kText);
  text.text = "Hello";
  text.style.text_size = kTextSize;
  UiNode column = vnmaki::make_node(NodeKind::kVbox);
  column.style.align = vnmaki::Align::kStart;
  column.children.push_back(text);
  game.host.show("test", column, false);
  const float before = game.host.find("test")->children.front().rect.height;

  CHECK(game.runtime.set_binding("prefs", "text_scale", kScale).empty());
  game.preferences.update();
  CHECK(game.host.text_scale() == doctest::Approx(kScale));
  const UiNode& shown = game.host.find("test")->children.front();
  CHECK(shown.style.text_size == doctest::Approx(kTextSize * kScale));
  CHECK(shown.rect.height == doctest::Approx(before * kScale));

  // A screen shown later is scaled too, and scale 1 gives the sizes back.
  game.host.show("later", column, false);
  CHECK(game.host.find("later")->children.front().style.text_size ==
        doctest::Approx(kTextSize * kScale));
  CHECK(game.runtime.set_binding("prefs", "text_scale", 1).empty());
  game.preferences.update();
  CHECK(game.host.find("test")->children.front().style.text_size == doctest::Approx(kTextSize));

  CHECK(game.runtime.set_binding("prefs", "text_scale", kTooBig).empty());
  CHECK(vnmaki::read_preferences(game.runtime).text_scale == vnmaki::kMaxTextScale);
  CHECK(game.runtime.set_binding("prefs", "text_scale", kTooSmall).empty());
  CHECK(vnmaki::read_preferences(game.runtime).text_scale == vnmaki::kMinTextScale);
}

TEST_CASE("text scale: a line that fitted wraps, and the layout reflows") {
  vnmaki::Vfs vfs;
  REQUIRE(
      vfs.mount(vnmaki::make_directory_mount(fs::path{VNMAKI_FIXTURES_DIR} / "text")));
  const vnmaki::Font font = vnmaki::load_font(vfs, "fonts/boxes.ttf");
  vnmaki::test::RecordingRenderer renderer;
  vnmaki::UiRenderer ui{renderer, font};

  UiNode line = vnmaki::make_node(NodeKind::kText);
  line.text = "aaa aaa aaa aaa";
  line.style.text_size = kTextSize;
  const float one_line_width = ui.text_measure()(line, kFlatWidth).width;
  line.style.width = one_line_width * kWrapMargin;

  UiNode column = vnmaki::make_node(NodeKind::kVbox);
  column.style.align = vnmaki::Align::kStart;
  column.children.push_back(line);
  vnmaki::UiHost host{kCanvas};
  host.set_text_measure(ui.text_measure());
  host.show("line", column, false);
  const float height = host.find("line")->children.front().rect.height;
  host.set_text_scale(kBigScale);
  CHECK(host.find("line")->children.front().rect.height > kBigScale * height);  // a line came
}

TEST_CASE("text scale: the say screen grows its box and moves the line down") {
  Game game{"say_scale", kStory};
  const vnmaki::ScreenNodeDef args{.kind = "args",
                                   .props = {{"speaker", std::string{"Eileen"}},
                                             {"text", std::string{"Hello"}}}};
  UiNode plain = game.build("say", args);
  vnmaki::layout_tree(plain, kCanvas, vnmaki::estimate_text_size);
  const UiNode* plain_line = find_node(plain, [](const UiNode& n) { return n.reveal; });
  REQUIRE(plain_line != nullptr);

  CHECK(game.runtime.set_binding("prefs", "text_scale", kDoubled).empty());
  UiNode big = game.build("say", args);
  vnmaki::layout_tree(big, kCanvas, vnmaki::estimate_text_size);
  const UiNode* big_line = find_node(big, [](const UiNode& n) { return n.reveal; });
  REQUIRE(big_line != nullptr);
  const UiNode* panel = find_node(big, [](const UiNode& n) { return n.style.background.has_value(); });
  const UiNode* plain_panel = find_node(plain, [](const UiNode& n) { return n.style.background.has_value(); });
  REQUIRE(panel != nullptr);
  REQUIRE(plain_panel != nullptr);
  CHECK(panel->rect.height > plain_panel->rect.height);
  CHECK(panel->rect.y < plain_panel->rect.y);  // it grows upward
  // The line sits lower inside the box: the name line is taller.
  CHECK(big_line->rect.y - panel->rect.y > plain_line->rect.y - plain_panel->rect.y);
}

TEST_CASE("text scale: the say box draws bigger glyphs") {
  vnmaki::Vfs vfs;
  REQUIRE(
      vfs.mount(vnmaki::make_directory_mount(fs::path{VNMAKI_FIXTURES_DIR} / "text")));
  const vnmaki::Font font = vnmaki::load_font(vfs, "fonts/boxes.ttf");
  vnmaki::test::RecordingRenderer renderer;
  vnmaki::SayBoxView view{renderer, font, vnmaki::SayBoxStyle::for_resolution(kCanvas)};
  const vnmaki::DialogueLine line{.speaker = {}, .color = std::nullopt, .text = "abc"};

  const auto tallest_glyph = [&] {
    renderer.draws.clear();
    view.draw(line, view.glyph_count(line), 1);
    float tallest = 0;
    for (const auto& draw : renderer.draws) {
      if (draw.quad.uv != vnmaki::kWholeTexture) {
        tallest = std::max(tallest, draw.quad.corners[3].y - draw.quad.corners[0].y);
      }
    }
    return tallest;
  };
  const float plain = tallest_glyph();
  REQUIRE(plain > 0);
  view.set_text_scale(kBigScale);
  CHECK(tallest_glyph() == doctest::Approx(kBigScale * plain).epsilon(kGlyphRounding));
}

TEST_CASE("preferences screen: the accessibility controls are bound to the preferences") {
  Game game{"prefs_a11y", kStory};
  game.open("@open:preferences");
  REQUIRE(game.host.is_shown("preferences"));

  game.click_on("preferences", "High contrast");
  game.click_on("preferences", "Dyslexia-friendly font");
  game.click_on("preferences", "Captions");
  game.click_on("preferences", "Reduce motion");
  CHECK(game.flag("high_contrast"));
  CHECK(game.flag("dyslexia_font"));
  CHECK(game.flag("captions"));
  CHECK(game.flag("reduce_motion"));
  game.preferences.update();
  CHECK(game.dyslexia_calls == std::vector<bool>{true});

  const UiNode* size = game.node("preferences", "Text size");
  REQUIRE(size != nullptr);
  game.click(center_of(*size));  // the middle of 0.75 to 2
  CHECK(game.number("text_scale") ==
        doctest::Approx((vnmaki::kMinTextScale + vnmaki::kMaxTextScale) / kDoubled).epsilon(kTenth));

  // The screen fits the canvas.
  const UiNode* root = game.host.find("preferences");
  CHECK(root->rect.height <= static_cast<float>(kCanvas.height));
}

// --- High contrast -----------------------------------------------------------------------------

TEST_CASE("high contrast: the theme goes over the game's styles and comes off again") {
  Game game{"contrast", R"(
style("mine", { size = 11 })
screen("mine_screen", function() return text { "x", style = "mine" } end)
label("start", function() narrate("x") end)
)"};
  const vnmaki::ScreenNodeDef args{.kind = "args", .props = {{"title", std::string{"Game"}}}};
  const auto start_button = [&] {
    UiNode title = game.build("title", args);
    UiNode* found = find_node(title, [](const UiNode& n) { return n.text == "Start"; });
    REQUIRE(found != nullptr);
    return *found;
  };
  const UiNode before = start_button();
  CHECK(before.style.border_width == 0);

  CHECK(game.runtime.set_binding("prefs", "high_contrast", true).empty());
  const UiNode during = start_button();
  CHECK(during.style.border_width > 0);
  CHECK(during.style.focus_background.has_value());
  CHECK(during.style.focus_background != before.style.focus_background);
  CHECK(during.style.padding == before.style.padding);  // only what the theme names changes
  CHECK(during.style.text_size == before.style.text_size);
  CHECK(game.build("mine_screen").style.text_size == 11);  // the game's own style is still there

  CHECK(game.runtime.set_binding("prefs", "high_contrast", false).empty());
  const UiNode after = start_button();
  CHECK(after.style.border_width == 0);
  CHECK(after.style.focus_background == before.style.focus_background);
  CHECK(game.warnings.empty());
}

TEST_CASE("high contrast: a game without styles/high_contrast.lua gets a warning, not a failure") {
  class Warnings final : public vnmaki::ScriptObserver {
   public:
    void on_warning(std::string_view message) override { lines.emplace_back(message); }
    std::vector<std::string> lines;
  };
  Warnings warnings;
  vnmaki::UiHost host{kCanvas};
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({{"scripts/main.lua", kStory}});
  vnmaki::ScriptRuntime runtime{vfs,
                                {.observer = &warnings,
                                 .screens = &host,
                                 .canvas_width = kCanvas.width,
                                 .canvas_height = kCanvas.height}};
  runtime.load_scripts();
  runtime.start("start");
  CHECK(runtime.set_binding("prefs", "high_contrast", true).empty());
  vnmaki::ScreenNodeDef tree;
  CHECK(runtime.build_screen("title", {.kind = "args"}, tree).empty());
  CHECK(runtime.build_screen("title", {.kind = "args"}, tree).empty());
  REQUIRE(warnings.lines.size() == 1);  // once
  CHECK(warnings.lines.front().find("high_contrast") != std::string::npos);
}

// --- Dyslexia-friendly font --------------------------------------------------------------------

TEST_CASE("dyslexia font: it comes first, and the old fonts follow as fallbacks") {
  vnmaki::Vfs vfs;
  REQUIRE(
      vfs.mount(vnmaki::make_directory_mount(fs::path{VNMAKI_FIXTURES_DIR} / "text")));
  const vnmaki::Font regular = vnmaki::load_font(vfs, "fonts/boxes.ttf");
  const vnmaki::Font dyslexia = vnmaki::load_font(vfs, "fonts/boxes_bold.ttf");
  const vnmaki::Font greek = vnmaki::load_font(vfs, "fonts/fallback.ttf");
  vnmaki::FontSet fonts{.base = {.regular = &regular}};
  fonts.fallbacks.push_back(&greek);

  const vnmaki::StyledText text = vnmaki::plain_text("a\xCE\xB1");  // a and Greek alpha
  const auto fonts_used = [&](const vnmaki::FontSet& set) {
    const vnmaki::TextLayout layout = vnmaki::layout_text(set, text, kTextSize, kFlatWidth);
    std::vector<const vnmaki::Font*> used;
    for (const vnmaki::TextLine& row : layout.lines) {
      for (const vnmaki::LayoutGlyph& glyph : row.glyphs) {
        used.push_back(layout.fonts.at(glyph.font));
      }
    }
    return used;
  };
  CHECK(fonts_used(fonts) == std::vector<const vnmaki::Font*>{&regular, &greek});
  CHECK(fonts_used(vnmaki::with_dyslexia_font(fonts, dyslexia)) ==
        std::vector<const vnmaki::Font*>{&dyslexia, &greek});
}

TEST_CASE("dyslexia font: the template has fonts/dyslexia.ttf, and it loads") {
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(template_dir())));
  REQUIRE(vfs.exists("fonts/dyslexia.ttf"));
  const vnmaki::Font regular = vnmaki::load_font(vfs, "fonts/default.ttf");
  const vnmaki::Font dyslexia = vnmaki::load_font(vfs, "fonts/dyslexia.ttf");
  const vnmaki::FontSet set = vnmaki::with_dyslexia_font({.base = {.regular = &regular}}, dyslexia);
  const vnmaki::TextLayout wide = vnmaki::layout_text(set, vnmaki::plain_text("Hello"), kTextSize,
                                                      kFlatWidth);
  const vnmaki::TextLayout narrow = vnmaki::layout_text(
      vnmaki::FontSet{.base = {.regular = &regular}}, vnmaki::plain_text("Hello"), kTextSize,
      kFlatWidth);
  CHECK(wide.lines.front().width > narrow.lines.front().width);  // the wider spacing
}

TEST_CASE("dyslexia font: the preference reaches the callback when it changes") {
  Game game{"dyslexia", kStory};
  game.preferences.update();
  CHECK(game.dyslexia_calls.empty());
  CHECK(game.runtime.set_binding("prefs", "dyslexia_font", true).empty());
  game.preferences.update();
  game.preferences.update();
  CHECK(game.runtime.set_binding("prefs", "dyslexia_font", false).empty());
  game.preferences.update();
  CHECK(game.dyslexia_calls == std::vector<bool>{true, false});
}

// --- Reduced motion ----------------------------------------------------------------------------

namespace {

// A story that shows a sprite with a slow slide, with the preference set or not.
struct Stage {
  explicit Stage(bool reduced, const std::string& transition = "{ \"slide_left\", duration = 2 }")
      : vfs(vnmaki::test::memory_vfs(
            {{"scripts/main.lua",
              std::string{"persistent.prefs = { reduce_motion = "} + (reduced ? "true" : "false") +
                  " }\nlabel(\"start\", function()\n  show(\"eileen\", { with = " + transition +
                  " })\n  narrate(\"done\")\nend)\n"},
             {"images/eileen.png", ""}})),
        scene(vfs, kCanvas),
        runtime(vfs, {.scene_state = &scene, .scene = &scene}) {
    runtime.load_scripts();
    runtime.start("start");
  }

  [[nodiscard]] bool transitioning() const { return scene.tweening(vnmaki::kTransitionTag); }

  vnmaki::Vfs vfs;
  vnmaki::Scene scene;
  vnmaki::ScriptRuntime runtime;
};

}  // namespace

TEST_CASE("reduce motion: every transition is a short dissolve") {
  Stage normal{false};
  REQUIRE(normal.runtime.state() == vnmaki::RunState::kWaiting);
  normal.scene.update(kQuarter);
  CHECK(normal.transitioning());  // two seconds

  Stage reduced{true};
  REQUIRE_MESSAGE(reduced.runtime.state() == vnmaki::RunState::kWaiting, reduced.runtime.error());
  CHECK(reduced.transitioning());  // the wait is still a step, so a replay counts the same
  reduced.scene.update(kMoment);
  CHECK_FALSE(reduced.transitioning());

  // A long transition that is already short keeps its time; no time stays a cut.
  Stage brief{true, "{ \"fade\", duration = 0.1 }"};
  brief.scene.update(kTenth / 2);
  CHECK(brief.transitioning());
  brief.scene.update(kTenth);
  CHECK_FALSE(brief.transitioning());
}

TEST_CASE("reduce motion: a bad option is still an error, and a mask is not needed") {
  Stage bad{true, "{ \"fade\", duration = \"long\" }"};
  CHECK(bad.runtime.state() == vnmaki::RunState::kFailed);
  Stage masked{true, "{ \"mask\", mask = \"masks/missing\" }"};  // the dissolve takes its place
  CHECK(masked.runtime.state() == vnmaki::RunState::kWaiting);
}

// --- Captions ----------------------------------------------------------------------------------

namespace {

vnmaki::Vfs sound_files(const std::string& script) {
  return vnmaki::test::memory_vfs({{"scripts/main.lua", script},
                                   {"audio/sfx/door.wav", vnmaki::test::audio_fixture("tone.wav")},
                                   {"audio/bgm/theme.wav", vnmaki::test::audio_fixture("long.wav")},
                                   {"audio/bgm/night.wav", vnmaki::test::audio_fixture("long.wav")}});
}

constexpr const char* kSoundStory = R"(
label("start", function()
  sound("sfx/door", { caption = "a door slams" })
  music("bgm/theme", { caption = "calm music" })
  music("bgm/theme", { caption = "calm music again" })
  ambient("bgm/night", { caption = "night wind" })
  sound("sfx/door")
  narrate("done")
end)
)";

}  // namespace

TEST_CASE("captions: sound, music and ambient give captions for the length of the sound") {
  const vnmaki::Vfs vfs = sound_files(kSoundStory);
  vnmaki::Audio audio{vfs};
  vnmaki::ScriptRuntime runtime{vfs, {.audio = &audio}};
  runtime.load_scripts();
  runtime.start("start");
  REQUIRE_MESSAGE(runtime.state() == vnmaki::RunState::kWaiting, runtime.error());

  const std::vector<vnmaki::AudioCaption> captions = audio.take_captions();
  REQUIRE(captions.size() == kThreeCaptions);  // a repeated music and a sound without one: none
  CHECK(captions[0].text == "a door slams");
  CHECK(captions[0].seconds == doctest::Approx(kSoundSeconds));
  CHECK(captions[1].text == "calm music");
  CHECK(captions[1].seconds == vnmaki::kMusicCaptionSeconds);
  CHECK(captions[2].text == "night wind");
  CHECK(audio.channel("ambient").music == "bgm/night");  // `ambient` is music on that channel
  CHECK(audio.take_captions().empty());
}

TEST_CASE("captions: a replay makes none, and a bad caption is an error") {
  const vnmaki::Vfs vfs = sound_files(kSoundStory);
  vnmaki::Audio audio{vfs};
  vnmaki::ScriptRuntime runtime{vfs, {.audio = &audio}};
  runtime.load_scripts();
  runtime.start("start");
  static_cast<void>(audio.take_captions());
  REQUIRE(runtime.label_snapshot() != nullptr);
  const vnmaki::LabelSnapshot entry = *runtime.label_snapshot();
  runtime.replay(entry, {}, 1);
  REQUIRE_MESSAGE(runtime.state() == vnmaki::RunState::kWaiting, runtime.error());
  CHECK(audio.take_captions().empty());

  const vnmaki::Vfs bad_vfs = sound_files(R"(
label("start", function() sound("sfx/door", { caption = 5 }) end)
)");
  vnmaki::Audio bad_audio{bad_vfs};
  vnmaki::ScriptRuntime bad{bad_vfs, {.audio = &bad_audio}};
  bad.load_scripts();
  bad.start("start");
  CHECK(bad.state() == vnmaki::RunState::kFailed);
  CHECK(bad.error().find("caption") != std::string::npos);
}

TEST_CASE("captions: the screen shows them while the preference is on, for their time") {
  Game game{"captions", kStory};
  vnmaki::CaptionPresenter captions{game.runtime, game.host};
  const auto shown = [&](std::string_view text) {
    UiNode* root = game.host.find(vnmaki::kCaptionsScreenName);
    return root != nullptr &&
           find_node(*root, [&](const UiNode& n) { return n.text == text; }) != nullptr;
  };

  // Off: the captions are dropped.
  captions.update({{.text = "knock", .seconds = kSecond}}, kTenth, false);
  CHECK_FALSE(game.host.is_shown(vnmaki::kCaptionsScreenName));

  captions.update({{.text = "knock", .seconds = kSecond}, {.text = "wind", .seconds = kFive}},
                  kTenth, true);
  CHECK(shown("[knock]"));
  CHECK(shown("[wind]"));
  captions.update({}, kSecond, true);  // a second later the knock is over
  CHECK_FALSE(shown("[knock]"));
  CHECK(shown("[wind]"));
  captions.update({}, kFive, true);
  CHECK_FALSE(game.host.is_shown(vnmaki::kCaptionsScreenName));

  // Turning the preference off removes what shows.
  captions.update({{.text = "knock", .seconds = kFive}}, kTenth, true);
  REQUIRE(game.host.is_shown(vnmaki::kCaptionsScreenName));
  captions.update({}, kTenth, false);
  CHECK_FALSE(game.host.is_shown(vnmaki::kCaptionsScreenName));

  // At most kMaxCaptions show: the newest.
  std::vector<vnmaki::AudioCaption> many;
  for (const std::string_view text : {"a", "b", "c", "d", "e"}) {
    many.push_back({.text = std::string{text}, .seconds = kFive});
  }
  captions.update(std::move(many), kTenth, true);
  CHECK(captions.shown().size() == vnmaki::kMaxCaptions);
  CHECK(captions.shown().front() == "b");
}
