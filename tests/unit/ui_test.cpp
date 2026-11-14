// The UI module without a window or GPU: layout rectangles, focus order, input, and the menu and
// call_screen requests answered through the screens. Text is sized by estimate_text_size() (half
// an em for each character).

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/dialogue/say_box.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/viewport.hpp"
#include "engine/runtime/headless_runner.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/ui/screen_presenter.hpp"
#include "engine/ui/ui_build.hpp"
#include "engine/ui/ui_focus.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/ui/ui_layout.hpp"
#include "engine/ui/ui_node.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::FocusMove;
using vnmaki::NodeKind;
using vnmaki::PointF;
using vnmaki::RectF;
using vnmaki::ScreenValue;
using vnmaki::UiNode;

constexpr vnmaki::Size kCanvas{.width = 400, .height = 300};
constexpr float kPadding = 20;
constexpr float kGap = 10;
constexpr float kSmallGap = 5;
constexpr float kTextSize = 20;
constexpr float kBoxWidth = 50;
constexpr float kWideBoxWidth = 60;
constexpr float kBoxHeight = 30;
constexpr float kFarX = 300;
constexpr float kFarY = 250;
constexpr float kTinyWidth = 40;
constexpr float kTinyHeight = 10;
constexpr float kButtonWidth = 200;
constexpr float kButtonHeight = 40;
constexpr std::int64_t kSecondIndex = 1;
constexpr std::int64_t kThirdIndex = 2;
constexpr std::int64_t kSeven = 7;

UiNode box(float width, float height) {
  UiNode node = vnmaki::make_node(NodeKind::kBox);
  node.style.width = width;
  node.style.height = height;
  return node;
}

UiNode button(const std::string& caption, std::int64_t returns) {
  UiNode node = vnmaki::make_node(NodeKind::kButton);
  node.text = caption;
  node.label = caption;
  node.returns = returns;
  node.style.width = kButtonWidth;
  node.style.height = kButtonHeight;
  return node;
}

// Three buttons in a column, 10 apart, from the top-left corner.
UiNode column_of_buttons() {
  UiNode root = vnmaki::make_node(NodeKind::kVbox);
  root.style.gap = kGap;
  root.style.align = vnmaki::Align::kStart;
  root.children.push_back(button("one", 0));
  root.children.push_back(button("two", kSecondIndex));
  root.children.push_back(button("three", kThirdIndex));
  vnmaki::layout_tree(root, kCanvas, vnmaki::estimate_text_size);
  return root;
}

int focused_index(UiNode& root) {
  for (std::size_t i = 0; i < root.children.size(); ++i) {
    if (root.children[i].state.focused) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

vnmaki::KeyEvent press(vnmaki::Key key, bool shift = false) {
  return {.key = key, .pressed = true, .repeat = false, .mods = {.shift = shift}};
}

const vnmaki::Viewport kViewport =
    vnmaki::compute_viewport(kCanvas, kCanvas, vnmaki::ScaleMode::kFit);

PointF center_of(const UiNode& node) {
  return {.x = node.rect.x + (node.rect.width / 2), .y = node.rect.y + (node.rect.height / 2)};
}

// A runtime over one script with a UiHost for its screens.
struct Game {
  vnmaki::Vfs vfs;
  vnmaki::UiHost host{kCanvas};
  vnmaki::ScriptRuntime runtime;

  explicit Game(const char* script)
      : vfs(vnmaki::test::memory_vfs({{"scripts/main.lua", script}})),
        runtime(vfs, {.screens = &host}) {
    runtime.load_scripts();
    runtime.start("start");
  }

  [[nodiscard]] std::string narration() const {
    const vnmaki::BlockingRequest* request = runtime.request();
    const auto* narrate = request ? std::get_if<vnmaki::NarrateRequest>(request) : nullptr;
    return narrate ? narrate->text : std::string{};
  }
};

}  // namespace

TEST_CASE("ui: layout gives each node its rectangle") {
  UiNode root = vnmaki::make_node(NodeKind::kVbox);
  root.style.padding = kPadding;
  root.style.gap = kGap;
  root.style.align = vnmaki::Align::kStart;
  UiNode label = vnmaki::make_node(NodeKind::kText);
  label.text = "ab";
  label.style.text_size = kTextSize;
  UiNode row = vnmaki::make_node(NodeKind::kHbox);
  row.style.gap = kSmallGap;
  row.children.push_back(box(kBoxWidth, kBoxHeight));
  row.children.push_back(box(kWideBoxWidth, kBoxHeight));
  UiNode floating = box(kTinyWidth, kTinyHeight);
  floating.style.position = PointF{.x = kFarX, .y = kFarY};
  root.children.push_back(std::move(label));
  root.children.push_back(std::move(row));
  root.children.push_back(std::move(floating));

  vnmaki::layout_tree(root, kCanvas, vnmaki::estimate_text_size);

  const RectF everything{.x = 0,
                         .y = 0,
                         .width = static_cast<float>(kCanvas.width),
                         .height = static_cast<float>(kCanvas.height)};
  CHECK(root.rect == everything);
  CHECK(root.children[0].rect ==
        RectF{.x = kPadding, .y = kPadding, .width = kTextSize, .height = kTextSize});
  const float row_top = kPadding + kTextSize + kGap;
  CHECK(root.children[1].rect == RectF{.x = kPadding,
                                       .y = row_top,
                                       .width = kBoxWidth + kSmallGap + kWideBoxWidth,
                                       .height = kBoxHeight});
  CHECK(root.children[1].children[0].rect ==
        RectF{.x = kPadding, .y = row_top, .width = kBoxWidth, .height = kBoxHeight});
  CHECK(root.children[1].children[1].rect == RectF{.x = kPadding + kBoxWidth + kSmallGap,
                                                   .y = row_top,
                                                   .width = kWideBoxWidth,
                                                   .height = kBoxHeight});
  CHECK(root.children[2].rect ==
        RectF{.x = kFarX, .y = kFarY, .width = kTinyWidth, .height = kTinyHeight});
}

TEST_CASE("ui: every node has a role, a label and a state") {
  UiNode root = vnmaki::make_node(NodeKind::kVbox);
  UiNode caption = vnmaki::make_node(NodeKind::kText);
  UiNode picture = vnmaki::make_node(NodeKind::kImage);
  UiNode choice = button("Go", 0);
  CHECK(root.role == vnmaki::NodeRole::kGroup);
  CHECK(caption.role == vnmaki::NodeRole::kText);
  CHECK(picture.role == vnmaki::NodeRole::kImage);
  CHECK(choice.role == vnmaki::NodeRole::kButton);
  CHECK(choice.label == "Go");
  CHECK(choice.state == vnmaki::NodeState{});

  choice.state.disabled = true;
  CHECK_FALSE(vnmaki::is_focusable(choice));
}

TEST_CASE("ui: Tab and the arrow keys walk the focus in order") {
  UiNode root = column_of_buttons();
  CHECK(focused_index(root) == -1);

  CHECK(vnmaki::move_focus(root, FocusMove::kNext));
  CHECK(focused_index(root) == 0);
  CHECK(vnmaki::move_focus(root, FocusMove::kDown));
  CHECK(focused_index(root) == 1);
  CHECK(vnmaki::move_focus(root, FocusMove::kDown));
  CHECK(focused_index(root) == 2);
  CHECK_FALSE(vnmaki::move_focus(root, FocusMove::kDown));  // the last one: no wrap
  CHECK(focused_index(root) == 2);
  CHECK(vnmaki::move_focus(root, FocusMove::kNext));  // Tab wraps
  CHECK(focused_index(root) == 0);
  CHECK(vnmaki::move_focus(root, FocusMove::kPrevious));
  CHECK(focused_index(root) == 2);
  CHECK(vnmaki::move_focus(root, FocusMove::kUp));
  CHECK(focused_index(root) == 1);
  CHECK_FALSE(vnmaki::move_focus(root, FocusMove::kLeft));  // nothing lies left
}

TEST_CASE("ui: a disabled button is skipped") {
  UiNode root = column_of_buttons();
  root.children[1].state.disabled = true;
  CHECK(vnmaki::focusable_nodes(root).size() == 2);
  CHECK(vnmaki::move_focus(root, FocusMove::kNext));
  CHECK(vnmaki::move_focus(root, FocusMove::kNext));
  CHECK(focused_index(root) == 2);
}

TEST_CASE("ui: hovering a button focuses it") {
  UiNode root = column_of_buttons();
  const PointF over_second = center_of(root.children[1]);

  CHECK(vnmaki::hover_at(root, over_second));
  CHECK(focused_index(root) == 1);
  CHECK(root.children[1].state.hovered);

  CHECK_FALSE(vnmaki::hover_at(root, {.x = kFarX, .y = kFarY}));  // over nothing
  CHECK_FALSE(root.children[1].state.hovered);
  CHECK(focused_index(root) == 1);  // the focus stays
}

TEST_CASE("ui host: a modal screen takes keys, d-pad and pointer, and answers with the button") {
  vnmaki::UiHost host{kCanvas};
  host.show("choices", column_of_buttons(), true);
  REQUIRE(host.find("choices") != nullptr);
  CHECK(focused_index(*host.find("choices")) == 0);  // the first button starts focused

  auto result = host.handle_event(press(vnmaki::Key::kDown), kViewport);
  CHECK(result.consumed);
  CHECK_FALSE(result.activation.has_value());
  result = host.handle_event(
      vnmaki::GamepadButtonEvent{.button = vnmaki::GamepadButton::kDpadDown, .pressed = true},
      kViewport);
  CHECK(focused_index(*host.find("choices")) == 2);
  result = host.handle_event(press(vnmaki::Key::kTab), kViewport);
  CHECK(focused_index(*host.find("choices")) == 0);
  result = host.handle_event(press(vnmaki::Key::kTab, true), kViewport);
  CHECK(focused_index(*host.find("choices")) == 2);

  result = host.handle_event(press(vnmaki::Key::kEnter), kViewport);
  REQUIRE(result.activation.has_value());
  CHECK(result.activation->screen == "choices");
  CHECK(result.activation->value == ScreenValue{kThirdIndex});

  const PointF over_first = center_of(host.find("choices")->children[0]);
  result = host.handle_event(
      vnmaki::PointerMoveEvent{.position = vnmaki::virtual_to_window(kViewport, over_first)},
      kViewport);
  CHECK(focused_index(*host.find("choices")) == 0);
  result = host.handle_event(
      vnmaki::PointerButtonEvent{
          .button = vnmaki::MouseButton::kLeft, .pressed = true, .position = over_first},
      kViewport);
  CHECK(result.consumed);
  REQUIRE(result.activation.has_value());
  CHECK(result.activation->value == ScreenValue{std::int64_t{0}});

  // A click on nothing is swallowed while a modal screen is up.
  result = host.handle_event(vnmaki::PointerButtonEvent{.button = vnmaki::MouseButton::kLeft,
                                                        .pressed = true,
                                                        .position = {.x = kFarX, .y = kFarY}},
                             kViewport);
  CHECK(result.consumed);
  CHECK_FALSE(result.activation.has_value());
}

TEST_CASE("ui host: a screen that is not modal leaves the keys alone") {
  vnmaki::UiHost host{kCanvas};
  host.show("quick", column_of_buttons(), false);
  const auto result = host.handle_event(press(vnmaki::Key::kEnter), kViewport);
  CHECK_FALSE(result.consumed);
  CHECK_FALSE(host.has_modal());
}

TEST_CASE("ui host: showing a name again replaces the screen, hiding removes it") {
  vnmaki::UiHost host{kCanvas};
  host.show("a", column_of_buttons(), false);
  host.show("b", column_of_buttons(), false);
  host.show("a", UiNode{}, false);
  CHECK(host.roots().size() == 2);
  CHECK(host.hide("a"));
  CHECK_FALSE(host.hide("a"));
  CHECK(host.is_shown("b"));
}

TEST_CASE("ui build: a script's nodes become a tree with styles, and mistakes are named") {
  vnmaki::ScreenNodeDef def{
      .kind = "vbox",
      .props = {{"gap", std::int64_t{10}}, {"align", std::string{"center"}}},
      .children = {{.kind = "button",
                    .text = "Save",
                    .props = {{"bg", std::string{"#ff0000"}}, {"returns", std::string{"save"}}}}}};
  const UiNode built = vnmaki::build_ui_tree(def);
  CHECK(built.style.gap == kGap);
  CHECK(built.style.align == vnmaki::Align::kCenter);
  REQUIRE(built.children.size() == 1);
  CHECK(built.children[0].text == "Save");
  CHECK(built.children[0].label == "Save");
  CHECK(built.children[0].returns == std::optional<ScreenValue>{std::string{"save"}});
  CHECK(built.children[0].style.background == std::optional{vnmaki::rgba8(UINT8_MAX, 0, 0)});

  def.props.emplace_back("bogus", true);
  CHECK_THROWS_WITH_AS(static_cast<void>(vnmaki::build_ui_tree(def)),
                       "vbox: unknown option 'bogus'", vnmaki::UiBuildError);
  def.props.clear();
  def.kind = "image";
  CHECK_THROWS_AS(static_cast<void>(vnmaki::build_ui_tree(def)), vnmaki::UiBuildError);
  def.kind = "blob";
  CHECK_THROWS_AS(static_cast<void>(vnmaki::build_ui_tree(def)), vnmaki::UiBuildError);
}

TEST_CASE("ui: the say box is a screen with the panel, the name and the line in place") {
  const vnmaki::SayBoxStyle style = vnmaki::SayBoxStyle::for_resolution(kCanvas);
  const vnmaki::DialogueLine line{.speaker = "Eileen", .color = std::nullopt, .text = "Hi."};
  UiNode screen = vnmaki::make_say_screen(style, line);
  vnmaki::layout_tree(screen, kCanvas, vnmaki::estimate_text_size);

  REQUIRE(screen.children.size() == 1);
  const UiNode& panel = screen.children[0];
  CHECK(panel.rect == style.box);
  REQUIRE(panel.children.size() == 2);
  CHECK(panel.children[0].rect.x == style.box.x + style.name_offset.x);
  CHECK(panel.children[0].rect.y == style.box.y + style.name_offset.y);
  CHECK(panel.children[1].rect.x == style.box.x + style.text_offset.x);
  CHECK(panel.children[1].rect.width == style.wrap_width);
  CHECK(panel.children[1].markup);
}

TEST_CASE("menu: a menu shows a screen, and the chosen button answers with a 1-based index") {
  Game game{R"(
label("start", function()
  local c = menu { "Of course.", "My bag.", "Never." }
  narrate("picked " .. c)
end)
)"};
  vnmaki::ScreenPresenter screens{game.runtime, game.host};
  REQUIRE(screens.active());
  REQUIRE(game.host.is_shown(vnmaki::kMenuScreenName));
  UiNode* menu = game.host.find(vnmaki::kMenuScreenName);
  REQUIRE(menu->children.size() == 3);
  CHECK(menu->children[1].label == "My bag.");
  CHECK(menu->children[1].role == vnmaki::NodeRole::kButton);

  CHECK(screens.handle_event(press(vnmaki::Key::kDown), kViewport));
  CHECK(screens.handle_event(press(vnmaki::Key::kEnter), kViewport));

  CHECK(game.narration() == "picked 2");
  CHECK_FALSE(screens.active());
  CHECK_FALSE(game.host.is_shown(vnmaki::kMenuScreenName));
}

TEST_CASE("menu: a click on an option chooses it") {
  Game game{R"(
label("start", function()
  narrate("picked " .. menu { "a", "b" })
end)
)"};
  vnmaki::ScreenPresenter screens{game.runtime, game.host};
  const PointF second = center_of(game.host.find(vnmaki::kMenuScreenName)->children[1]);
  CHECK(screens.handle_event(
      vnmaki::PointerButtonEvent{
          .button = vnmaki::MouseButton::kLeft, .pressed = true, .position = second},
      kViewport));
  CHECK(game.narration() == "picked 2");
}

TEST_CASE("menu: the headless path still chooses by index") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({{"scripts/main.lua", R"(
label("start", function()
  narrate("picked " .. menu { "a", "b" })
end)
)"}});
  const std::vector<vnmaki::ScriptInput> inputs{vnmaki::ChooseInput{.index = 1},
                                                vnmaki::AdvanceInput{}};
  const vnmaki::HeadlessResult result = vnmaki::run_headless(vfs, inputs);
  CHECK(result.outcome == vnmaki::HeadlessOutcome::kFinished);
  CHECK(result.transcript.back() == "narrate picked 2");
}

TEST_CASE("screens: call_screen waits for the value a button returns") {
  constexpr const char* kScript = R"(
screen "pick" {
  vbox { gap = 8,
    text { "Which?", size = 20 },
    button { "Yes", returns = "yes" },
    button { "Seven", returns = 7 },
  }
}
label("start", function()
  local r = call_screen("pick")
  narrate("got " .. tostring(r) .. " " .. tostring(math.type(r)))
end)
)";
  SUBCASE("the first button") {
    Game game{kScript};
    vnmaki::ScreenPresenter screens{game.runtime, game.host};
    REQUIRE(game.host.is_shown("pick"));
    CHECK(game.host.has_modal());
    CHECK(screens.handle_event(press(vnmaki::Key::kSpace), kViewport));
    CHECK(game.narration() == "got yes nil");
    CHECK_FALSE(game.host.is_shown("pick"));
  }
  SUBCASE("the second button returns an integer") {
    Game game{kScript};
    vnmaki::ScreenPresenter screens{game.runtime, game.host};
    CHECK(screens.handle_event(press(vnmaki::Key::kDown), kViewport));
    CHECK(screens.handle_event(press(vnmaki::Key::kEnter), kViewport));
    CHECK(game.narration() == "got 7 integer");
  }
  SUBCASE("the runner returns a scripted value") {
    const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({{"scripts/main.lua", kScript}});
    const std::vector<vnmaki::ScriptInput> inputs{
        vnmaki::ReturnScreenInput{.value = ScreenValue{kSeven}}, vnmaki::AdvanceInput{}};
    const vnmaki::HeadlessResult result = vnmaki::run_headless(vfs, inputs);
    CHECK(result.outcome == vnmaki::HeadlessOutcome::kFinished);
    CHECK(result.transcript.at(1) == "screen pick");
    CHECK(result.transcript.back() == "narrate got 7 integer");
  }
}

TEST_CASE("screens: the answer of a call_screen goes in the input log and replays") {
  Game game{R"(
screen "pick" { button { "A", returns = "a" }, button { "B", returns = "b" } }
label("start", function()
  local r = call_screen("pick", {})
  narrate("got " .. r)
end)
)"};
  vnmaki::ScreenPresenter screens{game.runtime, game.host};
  CHECK(screens.handle_event(press(vnmaki::Key::kDown), kViewport));
  CHECK(screens.handle_event(press(vnmaki::Key::kEnter), kViewport));
  REQUIRE(game.narration() == "got b");

  const vnmaki::LabelSnapshot snapshot = *game.runtime.label_snapshot();
  const std::vector<vnmaki::InputResult> log{game.runtime.input_log().begin(),
                                             game.runtime.input_log().end()};
  REQUIRE(log.size() == 1);
  CHECK(log[0] == vnmaki::InputResult{vnmaki::ScreenResult{.value = ScreenValue{"b"}}});

  constexpr std::uint32_t kNarrateStep = 2;
  game.runtime.replay(snapshot, log, kNarrateStep);
  CHECK(game.narration() == "got b");
}

TEST_CASE("screens: show_screen and hide_screen change the host, arguments reach the screen") {
  Game game{R"(
screen("greet", function(args)
  return vbox { text { "Hi " .. args.name } }
end)
label("start", function()
  show_screen("greet", { name = "Eileen" })
  narrate("shown")
  hide_screen("greet")
  hide_screen("never shown")
  narrate("hidden")
end)
)"};
  REQUIRE(game.host.is_shown("greet"));
  CHECK_FALSE(game.host.has_modal());
  CHECK(game.host.find("greet")->children[0].text == "Hi Eileen");
  game.runtime.advance();
  CHECK_FALSE(game.host.is_shown("greet"));
  CHECK(game.narration() == "hidden");
}

TEST_CASE("screens: a mistake in a screen is a script error at its line") {
  Game game{R"(
label("start", function()
  call("other")
end)
label("other", function()
  show_screen("nope")
end)
)"};
  CHECK(game.runtime.state() == vnmaki::RunState::kFailed);
  CHECK(game.runtime.error().find("unknown screen 'nope'") != std::string::npos);

  Game bad{R"(
screen "s" { button { "x", bogus = 1 } }
label("start", function() show_screen("s") end)
)"};
  CHECK(bad.runtime.state() == vnmaki::RunState::kFailed);
  CHECK(bad.runtime.error().find("unknown option 'bogus'") != std::string::npos);
  CHECK(bad.runtime.error().find("main.lua:3") != std::string::npos);
}

namespace {

// Wheel up, Page Up and the left shoulder: what the story window rolls back on.
std::vector<vnmaki::InputEvent> rollback_events() {
  return {
      vnmaki::WheelEvent{.dy = 1}, press(vnmaki::Key::kPageUp),
      vnmaki::GamepadButtonEvent{.button = vnmaki::GamepadButton::kLeftShoulder, .pressed = true}};
}

}  // namespace

TEST_CASE("ui host: a modal screen consumes every input, a screen that is not modal none") {
  vnmaki::UiHost host{kCanvas};
  host.show("quick", column_of_buttons(), false);
  for (const vnmaki::InputEvent& event : rollback_events()) {
    CHECK_FALSE(host.handle_event(event, kViewport).consumed);
  }
  host.show("prefs", column_of_buttons(), true);
  for (const vnmaki::InputEvent& event : rollback_events()) {
    CHECK(host.handle_event(event, kViewport).consumed);
  }
  CHECK(host.handle_event(press(vnmaki::Key::kA), kViewport).consumed);
  CHECK(host.handle_event(press(vnmaki::Key::kL), kViewport).consumed);
}

TEST_CASE("menu: rollback input reaches the story at a menu, but not under another modal") {
  Game game{R"(
label("start", function()
  narrate("before")
  narrate("picked " .. menu { "a", "b" })
end)
)"};
  game.runtime.advance();
  vnmaki::ScreenPresenter screens{game.runtime, game.host};
  REQUIRE(screens.active());
  for (const vnmaki::InputEvent& event : rollback_events()) {
    CHECK_FALSE(screens.handle_event(event, kViewport));  // the story may roll back
  }
  game.host.show("save", column_of_buttons(), true);  // a save, load or confirm screen
  for (const vnmaki::InputEvent& event : rollback_events()) {
    CHECK(screens.handle_event(event, kViewport));
  }
}

TEST_CASE("menu: a replay to the same position with other options shows the new menu") {
  Game game{R"(
label("start", function()
  persistent.n = (persistent.n or 0) + 1
  menu { "opt" .. persistent.n }
end)
)"};
  vnmaki::ScreenPresenter screens{game.runtime, game.host};
  REQUIRE(game.host.find(vnmaki::kMenuScreenName)->children.at(0).label == "opt1");

  constexpr std::uint32_t kMenuStep = 1;
  const vnmaki::LabelSnapshot snapshot = *game.runtime.label_snapshot();
  game.runtime.replay(snapshot, {}, kMenuStep);  // as a quick-load of the same label and step
  screens.sync();
  CHECK(game.host.find(vnmaki::kMenuScreenName)->children.at(0).label == "opt2");
}
