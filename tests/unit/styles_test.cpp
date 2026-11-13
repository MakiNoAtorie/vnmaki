// UI styles, themes and the `input` node (docs/SPEC.md section 15) without a window: how a style
// merges with a node's own options, a theme that changes the rectangles, and an input that is
// typed into, edited with Backspace and left. Text is sized by estimate_text_size() (half an em
// for each character) on a 400 by 300 canvas.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/platform/geometry.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/platform.hpp"
#include "engine/platform/viewport.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/text/text.hpp"
#include "engine/ui/ui_build.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/ui/ui_layout.hpp"
#include "engine/ui/ui_node.hpp"
#include "engine/ui/ui_renderer.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"
#include "tests/support/recording_renderer.hpp"

namespace {

using vnmaki::Key;
using vnmaki::NodeKind;
using vnmaki::PointF;
using vnmaki::RunState;
using vnmaki::UiNode;

constexpr vnmaki::Size kCanvas{.width = 400, .height = 300};
const vnmaki::Viewport kViewport =
    vnmaki::compute_viewport(kCanvas, kCanvas, vnmaki::ScaleMode::kFit);

constexpr float kStylePadding = 7;
constexpr float kNodePadding = 3;
constexpr float kBaseGap = 4;
constexpr float kTextSize = 20;
constexpr float kWideWidth = 300;
constexpr float kNarrowWidth = 100;
constexpr float kThemedTextSize = 50;
constexpr float kQuickGap = 8.0F * 300 / 1080;  // 8 units of a canvas 1080 high, at 300
constexpr float kInputWidth = 150;
constexpr float kBorderWidth = 2;
constexpr std::size_t kMaxLength = 5;
constexpr float kHalfAlpha = 0.5F;
constexpr float kBoxSide = 80;
constexpr std::size_t kBorderQuads = 4;
constexpr std::size_t kShortLength = 3;
constexpr std::size_t kNodesInMerge = 5;

using Files = std::map<std::string, std::string, std::less<>>;

// A game of `scripts/main.lua` and the extra files, with a UiHost that shows its screens.
struct Game {
  vnmaki::Vfs vfs;
  vnmaki::UiHost host{kCanvas};
  vnmaki::HeadlessPlatform platform;
  vnmaki::ScriptRuntime runtime;

  Game(const char* script, const Files& more = {})
      : vfs(vnmaki::test::memory_vfs(with_script(script, more))),
        runtime(
            vfs,
            {.screens = &host, .canvas_width = kCanvas.width, .canvas_height = kCanvas.height}) {
    host.set_bindings(&runtime);
    host.set_platform(&platform);
    runtime.load_scripts();
    runtime.start("start");
  }

  static Files with_script(const char* script, Files files) {
    files["scripts/main.lua"] = script;
    return files;
  }

  vnmaki::UiEventResult send(const vnmaki::InputEvent& event) {
    return host.handle_event(event, kViewport);
  }

  vnmaki::UiEventResult type(const std::string& text) {
    return send(vnmaki::TextInputEvent{.text = text});
  }

  vnmaki::UiEventResult press(Key key) {
    return send(vnmaki::KeyEvent{.key = key, .pressed = true});
  }

  void click(PointF at) {
    send(vnmaki::PointerButtonEvent{
        .button = vnmaki::MouseButton::kLeft, .pressed = true, .position = at});
    send(vnmaki::PointerButtonEvent{
        .button = vnmaki::MouseButton::kLeft, .pressed = false, .position = at});
  }

  [[nodiscard]] std::string text(const char* key) const {
    const vnmaki::ScreenValue value = runtime.binding("store", key);
    const auto* held = std::get_if<std::string>(&value);
    return held != nullptr ? *held : std::string{"<not a string>"};
  }
};

// Why the game cannot run: the message of a script that fails to load or the error of the story.
std::string failure(const char* script, const Files& more = {}) {
  try {
    Game game{script, more};
    return game.runtime.state() == RunState::kFailed ? game.runtime.error() : std::string{};
  } catch (const vnmaki::ScriptError& error) {
    return error.what();
  }
}

PointF center_of(const UiNode& node) {
  return {.x = node.rect.x + (node.rect.width / 2), .y = node.rect.y + (node.rect.height / 2)};
}

constexpr const char* kMergeStory = R"(
style("base", { padding = 7, gap = 4, color = "#ff0000", size = 20 })
style("card", { extends = "base", padding = 9, background = "#00ff00",
                border = { width = 2, color = "#0000ff" },
                focus = { background = "#111111", color = "#222222" },
                hover = { background = "#333333" } })
label("start", function()
  show_screen("s")
  narrate("x")
end)
screen("s", function()
  return vbox {
    text { "plain", style = "base" },
    text { "own", style = "base", padding = 3 },
    text { "card", style = "card" },
    button { "button", style = "card" },
    input { style = "card", placeholder = "p" },
  }
end)
)";

}  // namespace

TEST_CASE("style: a style fills in options, the node's own options win, extends goes first") {
  Game game{kMergeStory};
  REQUIRE(game.runtime.state() == RunState::kWaiting);
  const UiNode* root = game.host.find("s");
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == kNodesInMerge);

  const UiNode& plain = root->children[0];
  CHECK(plain.style.padding == doctest::Approx(kStylePadding));
  CHECK(plain.style.gap == doctest::Approx(kBaseGap));
  CHECK(plain.style.text_size == doctest::Approx(kTextSize));
  CHECK(plain.style.color == vnmaki::rgba8(255, 0, 0));

  CHECK(root->children[1].style.padding == doctest::Approx(kNodePadding));  // the node wins
  CHECK(root->children[1].style.gap == doctest::Approx(kBaseGap));          // the style fills in

  const UiNode& card = root->children[2];
  CHECK(card.style.padding == doctest::Approx(kStylePadding + 2));  // "card" over "base"
  CHECK(card.style.color == vnmaki::rgba8(255, 0, 0));              // from "base"
  CHECK(card.style.background == vnmaki::rgba8(0, 255, 0));
  CHECK(card.style.border_width == doctest::Approx(kBorderWidth));
  CHECK(card.style.border_color == vnmaki::rgba8(0, 0, 255));
  CHECK(card.style.hover_background == vnmaki::rgba8(51, 51, 51));
  // A text node cannot take the focus, so the focus options stay out and do not fail.
  CHECK_FALSE(card.style.focus_background.has_value());

  const UiNode& button = root->children[3];
  CHECK(button.style.focus_background == vnmaki::rgba8(17, 17, 17));
  CHECK(button.style.focus_color == vnmaki::rgba8(34, 34, 34));
  // The style replaces the built-in look of a button.
  CHECK(button.style.background == vnmaki::rgba8(0, 255, 0));

  CHECK(root->children[4].kind == NodeKind::kInput);
  CHECK(root->children[4].style.focus_background == vnmaki::rgba8(17, 17, 17));
}

TEST_CASE("style: an unknown style, a loop and a bad option are errors at the script line") {
  const std::string unknown = failure(R"(
label("start", function()
  show_screen("s")
end)
screen("s", function() return vbox { text { "x", style = "nope" } } end)
)");
  CHECK(unknown.find("unknown style 'nope'") != std::string::npos);
  CHECK(unknown.find("main.lua:5") != std::string::npos);

  CHECK(failure(R"(
style("a", { extends = "b" })
style("b", { extends = "a" })
label("start", function()
  show_screen("s")
end)
screen("s", function() return vbox { text { "x", style = "a" } } end)
)")
            .find("extends itself") != std::string::npos);

  CHECK(failure(R"(
label("start", function() narrate("x") end)
style("a", { colour = "#ffffff" })
)")
            .find("unknown style option 'colour'") != std::string::npos);
}

TEST_CASE("theme: switching the theme changes the rectangles of the next screens") {
  constexpr const char* kStory = R"(
label("start", function()
  theme("wide")
  show_screen("s")
  narrate("a")
  theme("narrow")
  show_screen("s")
  narrate("b")
end)
screen("s", function() return vbox { align = "start", box { style = "block" } } end)
)";
  Game game{kStory,
            {{"styles/wide.lua", "style('block', { width = 300, height = 40 })"},
             {"styles/narrow.lua", "style('block', { width = 100, height = 40 })"}}};
  REQUIRE(game.runtime.state() == RunState::kWaiting);
  CHECK(game.host.find("s")->children[0].rect.width == doctest::Approx(kWideWidth));
  game.runtime.advance();
  REQUIRE(game.runtime.state() == RunState::kWaiting);
  CHECK(game.host.find("s")->children[0].rect.width == doctest::Approx(kNarrowWidth));
}

TEST_CASE("theme: a theme keeps the styles of the engine's screens and replaces the game's") {
  constexpr const char* kStory = R"(
style("mine", { size = 11 })
theme("restyle")
label("start", function()
  show_screen("quick_menu")
  narrate("a")
end)
)";
  Game game{kStory, {{"styles/restyle.lua", "style('quick_button', { size = 50 })"}}};
  REQUIRE(game.runtime.state() == RunState::kWaiting);
  const UiNode* menu = game.host.find("quick_menu");
  REQUIRE(menu != nullptr);
  const UiNode& row = menu->children[0];
  CHECK(row.style.gap == doctest::Approx(kQuickGap));  // "quick_row" is still the engine's
  CHECK(row.children[0].style.text_size == doctest::Approx(kThemedTextSize));  // the theme's

  // "mine" was declared before the theme: it is gone.
  CHECK(failure(R"(
style("mine", { size = 11 })
theme("restyle")
label("start", function()
  show_screen("s")
end)
screen("s", function() return vbox { text { "x", style = "mine" } } end)
)",
                {{"styles/restyle.lua", ""}})
            .find("unknown style 'mine'") != std::string::npos);
}

TEST_CASE("theme: a missing theme or a bad file is an error that names it") {
  CHECK(failure(R"(
theme("nowhere")
label("start", function() narrate("x") end)
)")
            .find("unknown theme 'nowhere'") != std::string::npos);

  CHECK(failure(R"(
theme("bad")
label("start", function() narrate("x") end)
)",
                {{"styles/bad.lua", "style('x', { colour = 1 })"}})
            .find("styles/bad.lua:1") != std::string::npos);
}

TEST_CASE("ui build: an input has a placeholder and a length, and a bad length is named") {
  const vnmaki::ScreenNodeDef field{
      .kind = "input",
      .props = {{"placeholder", "Name"}, {"max_length", static_cast<std::int64_t>(kShortLength)}}};
  const UiNode node = vnmaki::build_ui_tree(field);
  CHECK(node.kind == NodeKind::kInput);
  CHECK(node.role == vnmaki::NodeRole::kTextInput);
  CHECK(node.placeholder == "Name");
  CHECK(node.label == "Name");
  CHECK(node.max_length == kShortLength);
  CHECK(vnmaki::is_focusable(node));
  CHECK(vnmaki::display_text(node) == "Name");

  const vnmaki::ScreenNodeDef zero{.kind = "input", .props = {{"max_length", std::int64_t{0}}}};
  CHECK_THROWS_WITH_AS(static_cast<void>(vnmaki::build_ui_tree(zero)),
                       doctest::Contains("max_length"), vnmaki::UiBuildError);
}

TEST_CASE("input: typing, Backspace and a length limit edit the bound string") {
  Game game{R"(
label("start", function()
  store.name = ""
  show_screen("form")
  narrate("x")
end)
screen("form", function()
  return vbox {
    modal = true, align = "start",
    input { bind = bind(store, "name"), placeholder = "Name", max_length = 5, size = 20 },
    button { "Done", returns = "done" },
  }
end)
)"};
  REQUIRE(game.runtime.state() == RunState::kWaiting);
  CHECK(game.platform.text_input_enabled());  // a modal screen focuses its first node: the input

  const float empty_width = game.host.find("form")->children[0].rect.width;
  CHECK(game.host.find("form")->children[0].text.empty());
  CHECK(game.type("ab").consumed);
  CHECK(game.text("name") == "ab");
  CHECK(game.host.find("form")->children[0].text == "ab");
  CHECK(game.host.find("form")->children[0].rect.width != doctest::Approx(empty_width));

  CHECK(game.type("\xC3\xA9").consumed);  // one character of two bytes
  CHECK(game.text("name") == "ab\xC3\xA9");
  CHECK(game.press(Key::kBackspace).consumed);
  CHECK(game.text("name") == "ab");

  // At most max_length characters; a control character is dropped.
  CHECK(game.type("cde\nf").consumed);
  CHECK(game.text("name") == "abcde");
  CHECK(game.text("name").size() == kMaxLength);
  CHECK(game.type("z").consumed);
  CHECK(game.text("name") == "abcde");

  for (std::size_t i = 0; i < kMaxLength + 2; ++i) {  // more presses than characters
    CHECK(game.press(Key::kBackspace).consumed);
  }
  CHECK(game.text("name").empty());
  // An empty input shows its placeholder.
  CHECK(vnmaki::display_text(game.host.find("form")->children[0]) == "Name");
}

TEST_CASE("input: the keys are the input's while it has the focus, and text input follows it") {
  Game game{R"(
label("start", function()
  store.name = "x"
  show_screen("form")
  narrate("x")
end)
screen("form", function()
  return vbox {
    modal = true, align = "start",
    input { bind = bind(store, "name"), returns = "entered" },
    button { "Done", returns = "done" },
  }
end)
)"};
  REQUIRE(game.runtime.state() == RunState::kWaiting);
  REQUIRE(game.platform.text_input_enabled());

  // A letter, Space and a key release are for the input: the story must not use them.
  CHECK(game.press(Key::kA).consumed);
  CHECK(game.press(Key::kSpace).consumed);
  CHECK(game.send(vnmaki::KeyEvent{.key = Key::kS, .pressed = false}).consumed);
  CHECK(game.text("name") == "x");

  // Enter gives what `returns` says.
  const vnmaki::UiEventResult entered = game.press(Key::kEnter);
  REQUIRE(entered.activation.has_value());
  CHECK(std::get<std::string>(entered.activation->value) == "entered");

  // Tab leaves the input: its focus is lost, so text input goes off and typing is not consumed.
  CHECK(game.press(Key::kTab).consumed);
  CHECK_FALSE(game.platform.text_input_enabled());
  CHECK_FALSE(game.type("q").consumed);
  CHECK_FALSE(game.press(Key::kBackspace).consumed);
  CHECK(game.text("name") == "x");

  // Back to the input: on again. Hiding the screen turns it off.
  CHECK(game.press(Key::kTab).consumed);
  CHECK(game.platform.text_input_enabled());
  game.host.hide("form");
  CHECK_FALSE(game.platform.text_input_enabled());
}

TEST_CASE("input: a click gives a screen that is not modal the keys, Esc takes them back") {
  Game game{R"(
label("start", function()
  store.name = ""
  show_screen("form")
  narrate("x")
end)
screen("form", function()
  return vbox {
    align = "start",
    input { bind = bind(store, "name"), width = 150, size = 20 },
    button { "Done", returns = "done" },
  }
end)
)"};
  REQUIRE(game.runtime.state() == RunState::kWaiting);
  CHECK_FALSE(game.platform.text_input_enabled());  // nothing has the keys yet
  CHECK_FALSE(game.type("a").consumed);
  CHECK(game.text("name").empty());

  const UiNode* field = &game.host.find("form")->children[0];
  CHECK(field->rect.width == doctest::Approx(kInputWidth));
  game.click(center_of(*field));
  CHECK(game.platform.text_input_enabled());
  CHECK(game.type("hi").consumed);
  CHECK(game.text("name") == "hi");

  // Moving the pointer over the button does not take the focus from the input.
  const UiNode& done = game.host.find("form")->children[1];
  game.send(vnmaki::PointerMoveEvent{.position = center_of(done)});
  CHECK(game.platform.text_input_enabled());
  CHECK(game.type("!").consumed);
  CHECK(game.text("name") == "hi!");

  // Esc gives the keys back: the input loses the focus.
  CHECK(game.press(Key::kEscape).consumed);
  CHECK_FALSE(game.platform.text_input_enabled());
  CHECK_FALSE(game.type("?").consumed);
  CHECK(game.text("name") == "hi!");
}

namespace {

// The quads that UiRenderer draws for `root` laid out on the canvas, as a recording renderer saw
// them.
std::vector<vnmaki::test::RecordingRenderer::Draw> draw_tree(UiNode& root) {
  vnmaki::Vfs vfs;
  REQUIRE(
      vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_FIXTURES_DIR} / "text")));
  const vnmaki::Font font = vnmaki::load_font(vfs, "fonts/boxes.ttf");
  vnmaki::test::RecordingRenderer renderer;
  vnmaki::UiRenderer ui{renderer, font};
  vnmaki::layout_tree(root, kCanvas, ui.text_measure());
  ui.draw(root, 1.0F);
  return renderer.draws;
}

std::size_t count_tint(const std::vector<vnmaki::test::RecordingRenderer::Draw>& draws,
                       vnmaki::Color tint, bool whole_texture) {
  return static_cast<std::size_t>(std::ranges::count_if(draws, [&](const auto& draw) {
    return draw.quad.tint == tint && (draw.quad.uv == vnmaki::kWholeTexture) == whole_texture;
  }));
}

}  // namespace

TEST_CASE("style: the renderer draws a border, state colours, an input's caret and placeholder") {
  const vnmaki::Color red{.r = 1, .g = 0, .b = 0, .a = 1};
  const vnmaki::Color green{.r = 0, .g = 1, .b = 0, .a = 1};
  const vnmaki::Color blue{.r = 0, .g = 0, .b = 1, .a = 1};

  UiNode root = vnmaki::make_node(NodeKind::kVbox);
  root.style.align = vnmaki::Align::kStart;
  UiNode button = vnmaki::make_node(NodeKind::kButton);
  button.style.width = kBoxSide;
  button.style.height = kBoxSide;
  button.style.background = red;
  button.style.border_width = kBorderWidth;
  button.style.border_color = blue;
  button.style.focus_border_color = green;
  root.children.push_back(button);

  auto draws = draw_tree(root);
  CHECK(count_tint(draws, red, true) == 1);
  CHECK(count_tint(draws, blue, true) == kBorderQuads);
  CHECK(count_tint(draws, green, true) == 0);

  root.children[0].state.focused = true;  // the frame takes the focus colour
  draws = draw_tree(root);
  CHECK(count_tint(draws, blue, true) == 0);
  CHECK(count_tint(draws, green, true) == kBorderQuads);

  // A focused input has a caret (a whole-texture quad of its colour); one that is not has none.
  UiNode field_root = vnmaki::make_node(NodeKind::kVbox);
  field_root.style.align = vnmaki::Align::kStart;
  UiNode field = vnmaki::make_node(NodeKind::kInput);
  field.text = "ab";
  field_root.children.push_back(field);
  CHECK(count_tint(draw_tree(field_root), vnmaki::kWhite, true) == 0);
  field_root.children[0].state.focused = true;
  CHECK(count_tint(draw_tree(field_root), vnmaki::kWhite, true) == 1);

  // The placeholder of an empty input is drawn dim.
  field_root.children[0].text.clear();
  field_root.children[0].placeholder = "ab";
  field_root.children[0].state.focused = false;
  const vnmaki::Color dim{.r = 1, .g = 1, .b = 1, .a = kHalfAlpha};
  CHECK(count_tint(draw_tree(field_root), dim, false) > 0);
  CHECK(count_tint(draw_tree(field_root), vnmaki::kWhite, false) == 0);
}
