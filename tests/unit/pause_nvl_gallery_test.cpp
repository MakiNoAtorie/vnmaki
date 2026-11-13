// `pause`, NVL mode and the gallery (docs/SPEC.md sections 7, 8 and 15) without a window: the
// pause step with a click, skip mode, replay and rollback; the NVL page across rollback and load;
// unlocks after `show`; and the gallery screen with the mouse, the keyboard and a gamepad.
// Expected values are written out by hand from the script text.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/anim/wait_presenter.hpp"
#include "engine/dialogue/backlog.hpp"
#include "engine/dialogue/dialogue_presenter.hpp"
#include "engine/dialogue/playback.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/viewport.hpp"
#include "engine/render/renderer.hpp"
#include "engine/runtime/headless_runner.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/saves/persistent_store.hpp"
#include "engine/saves/saves.hpp"
#include "engine/ui/screen_actions.hpp"
#include "engine/ui/screen_presenter.hpp"
#include "engine/ui/ui_build.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/ui/ui_layout.hpp"
#include "engine/ui/ui_node.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"
#include "tests/support/temp_dir.hpp"

namespace {

using vnmaki::NodeKind;
using vnmaki::NvlPage;
using vnmaki::UiNode;
using Texts = std::vector<std::string>;

constexpr vnmaki::Size kCanvas{.width = 1920, .height = 1080};
const vnmaki::Viewport kViewport =
    vnmaki::compute_viewport(kCanvas, kCanvas, vnmaki::ScaleMode::kFit);

constexpr double kSecond = 1;
constexpr double kHalfSecond = 0.5;
constexpr double kLongPause = 100;
constexpr std::size_t kGlyphs = 20;
constexpr std::size_t kGalleryImages = 3;
constexpr double kFrame = 1;  // the playback frames below are as long as a second
constexpr std::uint8_t kLilacRed = 0xc8;  // 0xc8a2c8, the colour of the NVL test character
constexpr std::uint8_t kLilacGreen = 0xa2;
constexpr std::uint8_t kLilacBlue = 0xc8;
constexpr std::uint32_t kLilac = 0xc8a2c8;

// A scene that accepts every image, so the tests need no image files.
class AnyScene final : public vnmaki::SceneCommands {
 public:
  [[nodiscard]] std::string scene(std::string_view /*image*/) override { return {}; }
  [[nodiscard]] std::string show(const vnmaki::ShowRequest& /*request*/) override { return {}; }
  void hide(std::string_view /*tag*/) override {}
};

// A directory under the system temp directory, empty at the start and removed at the end.
class TempDir {
 public:
  explicit TempDir(const std::string& name)
      : path_(vnmaki::test::unique_temp_path("vnmaki_pause_nvl_test_", name)) {
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

// The node (depth first) under `root` that `matches`.
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

std::size_t count_nodes(UiNode& root, const std::function<bool(const UiNode&)>& matches) {
  std::size_t count = matches(root) ? 1 : 0;
  for (UiNode& child : root.children) {
    count += count_nodes(child, matches);
  }
  return count;
}

bool has_text(const vnmaki::ScreenNodeDef& node, std::string_view text) {
  if (node.text == text) {
    return true;
  }
  for (const vnmaki::ScreenNodeDef& child : node.children) {
    if (has_text(child, text)) {
      return true;
    }
  }
  return false;
}

// Everything a story window has for these features, minus the window and the GPU.
struct Game {
  TempDir dir;
  std::vector<std::string> warnings;
  vnmaki::Vfs vfs;
  AnyScene scene;
  vnmaki::UiHost host{kCanvas};
  vnmaki::ScriptRuntime runtime;
  vnmaki::PersistentStore persistent;
  vnmaki::Saves saves;
  vnmaki::ScreenPresenter screens;
  vnmaki::ScreenActions actions;
  vnmaki::WaitPresenter waits;
  vnmaki::DialoguePresenter dialogue;
  vnmaki::Playback playback;

  Game(const std::string& name, const char* script, std::vector<std::string> gallery = {})
      : dir(name),
        vfs(vnmaki::test::memory_vfs({{"scripts/main.lua", script}})),
        runtime(vfs,
                {.scene = &scene,
                 .screens = &host,
                 .gallery = std::move(gallery),
                 .canvas_width = kCanvas.width,
                 .canvas_height = kCanvas.height}),
        persistent(runtime, dir.path(), [this](std::string_view m) { warnings.emplace_back(m); }),
        saves(start(), {.directory = dir.path()}),
        screens(runtime, host),
        actions(runtime, host, saves,
                {.on_warning = [this](std::string_view m) { warnings.emplace_back(m); }}),
        waits(runtime, scene),
        dialogue(runtime, [](const vnmaki::DialogueLine&) { return kGlyphs; }),
        playback(runtime, dialogue, persistent,
                 {.skip_wait = [this] { return waits.skip(); }}) {
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
  void press(vnmaki::Key key) { send(vnmaki::KeyEvent{.key = key, .pressed = true}); }
  void tap(vnmaki::GamepadButton button) {
    send(vnmaki::GamepadButtonEvent{.button = button, .pressed = true});
  }

  void click_on(const UiNode& node) {
    const vnmaki::PointF at{.x = node.rect.x + (node.rect.width / 2),
                            .y = node.rect.y + (node.rect.height / 2)};
    send(vnmaki::PointerButtonEvent{
        .button = vnmaki::MouseButton::kLeft, .pressed = true, .position = at});
    send(vnmaki::PointerButtonEvent{
        .button = vnmaki::MouseButton::kLeft, .pressed = false, .position = at});
  }

  // The text of the narrate line the story waits at, "pause", "menu" or "other".
  [[nodiscard]] std::string at() const {
    const vnmaki::BlockingRequest* request = runtime.request();
    if (request == nullptr) {
      return "none";
    }
    if (const auto* narrate = std::get_if<vnmaki::NarrateRequest>(request)) {
      return narrate->text;
    }
    if (const auto* say = std::get_if<vnmaki::SayRequest>(request)) {
      return say->text;
    }
    if (const auto* wait = std::get_if<vnmaki::WaitRequest>(request)) {
      return wait->tag == vnmaki::kPauseTag ? "pause" : "wait";
    }
    return std::holds_alternative<vnmaki::MenuRequest>(*request) ? "menu" : "other";
  }

  // The texts of the NVL page the request carries; "-" when the request is not an NVL one.
  [[nodiscard]] Texts page() const {
    const vnmaki::BlockingRequest* request = runtime.request();
    const vnmaki::NvlPage* held = nullptr;
    if (const auto* say = std::get_if<vnmaki::SayRequest>(request)) {
      held = say->nvl ? &*say->nvl : nullptr;
    } else if (const auto* narrate = std::get_if<vnmaki::NarrateRequest>(request)) {
      held = narrate->nvl ? &*narrate->nvl : nullptr;
    } else if (const auto* menu = std::get_if<vnmaki::MenuRequest>(request)) {
      held = menu->nvl ? &*menu->nvl : nullptr;
    }
    if (held == nullptr) {
      return {"-"};
    }
    Texts texts;
    texts.reserve(held->size());
    for (const vnmaki::NvlLine& line : *held) {
      texts.push_back(line.text);
    }
    return texts;
  }
};

constexpr const char* kPauseStory = R"(
label("start", function()
  narrate("a")
  pause(2)
  narrate("b")
  pause(0.5)
  pause(1)
  narrate("c")
end)
)";

constexpr const char* kNvlStory = R"(
local eileen = character { name = "Eileen", color = "#c8a2c8" }
local lucy = character { name = "Lucy", kind = "nvl" }
label("start", function()
  say(eileen, "adv")
  nvl(true)
  say(eileen, "one")
  narrate("two")
  nvl_clear()
  say(lucy, "three")
  store.choice = menu { "a", "b" }
  nvl(false)
  say(eileen, "plain")
  say(lucy, "kind")
end)
)";

}  // namespace

// --- pause -------------------------------------------------------------------------------------

TEST_CASE("pause: it is a wait step that ends when its seconds have passed") {
  Game game{"pause_time", kPauseStory};
  REQUIRE(game.at() == "a");
  game.runtime.advance();

  REQUIRE(game.at() == "pause");
  CHECK(game.runtime.position() == vnmaki::ScriptPosition{.label = "start", .step = 2, .wait = true});
  CHECK(std::get<vnmaki::WaitRequest>(*game.runtime.request()).seconds == 2);
  CHECK(game.waits.waiting());
  CHECK_FALSE(game.waits.update(kSecond));
  CHECK(game.at() == "pause");
  CHECK(game.waits.update(kSecond));
  CHECK(game.at() == "b");
  CHECK(game.runtime.position() == vnmaki::ScriptPosition{.label = "start", .step = 3});

  // Two pauses in a row: one ends in each call, and the second starts its time again.
  game.runtime.advance();
  REQUIRE(game.at() == "pause");
  CHECK(game.waits.update(kHalfSecond));
  REQUIRE(game.at() == "pause");
  CHECK_FALSE(game.waits.update(kHalfSecond));
  CHECK(game.waits.update(kHalfSecond));
  CHECK(game.at() == "c");
}

TEST_CASE("pause: a click ends it at once") {
  Game game{"pause_click", kPauseStory};
  game.runtime.advance();
  REQUIRE(game.at() == "pause");

  CHECK(game.waits.skip());  // the story window calls this before it advances the dialogue
  CHECK(game.at() == "b");
  CHECK_FALSE(game.waits.skip());  // not at a wait any more: the click is a normal advance
}

TEST_CASE("pause: skip mode goes past it without time passing") {
  Game game{"pause_skip", R"(
label("start", function()
  narrate("a")
  pause(100)
  narrate("b")
end)
)"};
  game.playback.set_skip(vnmaki::SkipMode::kAll);
  game.runtime.advance();
  REQUIRE(game.at() == "pause");
  CHECK(std::get<vnmaki::WaitRequest>(*game.runtime.request()).seconds == kLongPause);

  game.dialogue.update(kFrame);  // as a frame of the story window does: presenter, then playback
  game.playback.update(kFrame);
  CHECK(game.at() == "b");
}

TEST_CASE("pause: rollback goes over it like over a wait, and replay does not wait for it") {
  Game game{"pause_rollback", kPauseStory};
  game.runtime.advance();
  game.waits.skip();
  REQUIRE(game.at() == "b");
  game.runtime.advance();
  game.waits.skip();
  game.waits.skip();
  REQUIRE(game.at() == "c");

  // The replays run through the three pauses with no clock at all.
  REQUIRE(game.runtime.roll_back());
  CHECK(game.at() == "b");
  REQUIRE(game.runtime.roll_back());
  CHECK(game.at() == "a");
  CHECK_FALSE(game.runtime.roll_back());
}

TEST_CASE("pause: the headless runner prints it and does not wait; a bad argument is an error") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({{"scripts/main.lua", kPauseStory}});
  const vnmaki::HeadlessResult result = vnmaki::run_headless(vfs, std::vector{vnmaki::ScriptInput{
                                                                      vnmaki::AdvanceInput{}}});
  CHECK(result.transcript ==
        Texts{"label start", "narrate a", "pause 2", "narrate b", "pause 0.5", "pause 1",
              "narrate c"});

  Game bad{"pause_bad", R"(
label("start", function()
  pause(-1)
end)
)"};
  CHECK(bad.runtime.state() == vnmaki::RunState::kFailed);
  CHECK(bad.runtime.error().find("bad argument #1 to 'pause'") != std::string::npos);
}

// --- NVL mode ----------------------------------------------------------------------------------

TEST_CASE("nvl: lines join the page in NVL mode and for an nvl character; nvl_clear empties it") {
  Game game{"nvl_page", kNvlStory};
  REQUIRE(game.at() == "adv");
  CHECK(game.page() == Texts{"-"});
  game.runtime.advance();

  REQUIRE(game.at() == "one");
  CHECK(game.page() == Texts{"one"});
  game.runtime.advance();
  REQUIRE(game.at() == "two");
  CHECK(game.page() == Texts{"one", "two"});
  CHECK(std::get<vnmaki::NarrateRequest>(*game.runtime.request()).nvl->back().speaker.empty());
  game.runtime.advance();
  REQUIRE(game.at() == "three");  // after nvl_clear()
  CHECK(game.page() == Texts{"three"});
  const auto& say = std::get<vnmaki::SayRequest>(*game.runtime.request());
  CHECK(say.nvl->back().speaker == "Lucy");
  game.runtime.advance();

  REQUIRE(game.at() == "menu");  // in NVL mode: the menu carries the page
  CHECK(game.page() == Texts{"three"});
  game.runtime.choose(1);
  REQUIRE(game.at() == "plain");  // nvl(false): back to the say box, page gone
  CHECK(game.page() == Texts{"-"});
  game.runtime.advance();
  REQUIRE(game.at() == "kind");  // an nvl character shows on the NVL screen in ADV mode
  CHECK(game.page() == Texts{"kind"});
}

TEST_CASE("nvl: rollback shows the same page again, also across nvl_clear") {
  Game game{"nvl_rollback", kNvlStory};
  for (int i = 0; i < 3; ++i) {  // adv, one, two: nvl_clear runs before "three"
    game.runtime.advance();
  }
  REQUIRE(game.at() == "three");

  REQUIRE(game.runtime.roll_back());
  CHECK(game.at() == "two");
  CHECK(game.page() == Texts{"one", "two"});
  REQUIRE(game.runtime.roll_back());
  CHECK(game.page() == Texts{"one"});
  REQUIRE(game.runtime.roll_back());
  CHECK(game.at() == "adv");
  CHECK(game.page() == Texts{"-"});
  game.runtime.advance();
  game.runtime.advance();
  CHECK(game.page() == Texts{"one", "two"});  // the page grew again, with no leftover
}

TEST_CASE("nvl: a save keeps the page, and load shows it again") {
  Game game{"nvl_load", kNvlStory};
  game.runtime.advance();
  game.runtime.advance();
  REQUIRE(game.at() == "two");
  REQUIRE(game.saves.save("1").empty());
  game.runtime.advance();
  game.runtime.advance();
  REQUIRE(game.at() == "menu");
  game.runtime.choose(0);
  REQUIRE(game.at() == "plain");

  REQUIRE(game.saves.load("1").empty());
  CHECK(game.at() == "two");
  CHECK(game.page() == Texts{"one", "two"});
  game.runtime.advance();
  CHECK(game.page() == Texts{"three"});
}

TEST_CASE("nvl: the backlog lists NVL lines like any other") {
  Game game{"nvl_backlog", kNvlStory};
  for (int i = 0; i < 3; ++i) {
    game.runtime.advance();
  }
  const vnmaki::Backlog backlog{game.runtime};
  Texts lines;
  for (const vnmaki::BacklogEntry& entry : backlog.entries()) {
    lines.push_back(entry.text);
  }
  CHECK(lines == Texts{"adv", "one", "two", "three"});
}

TEST_CASE("nvl: the presenter's line carries the page, so a same-text line is a new line") {
  Game game{"nvl_line", R"(
label("start", function()
  nvl(true)
  narrate("...")
  narrate("...")
end)
)"};
  const std::optional<vnmaki::DialogueLine> first = game.dialogue.line();
  REQUIRE(first.has_value());
  REQUIRE(first->nvl.has_value());
  CHECK(first->nvl->size() == 1);
  game.runtime.advance();
  game.dialogue.update(0);
  const std::optional<vnmaki::DialogueLine>& second = game.dialogue.line();
  REQUIRE(second.has_value());
  CHECK(second->nvl->size() == 2);
  CHECK(first != second);
}

TEST_CASE("nvl screen: the last line is the one on the typewriter") {
  Game game{"nvl_screen", kNvlStory};
  const vnmaki::NvlPage page{{.speaker = "Eileen", .text = "one", .color = kLilac},
                             {.speaker = "", .text = "two {i}there{/i}"}};
  vnmaki::ScreenNodeDef tree;
  REQUIRE(game.runtime.build_screen("nvl", vnmaki::make_nvl_args(page, true), tree).empty());
  UiNode screen = vnmaki::build_ui_tree(tree);
  vnmaki::layout_tree(screen, kCanvas, vnmaki::estimate_text_size);

  const UiNode* reveal = find_node(screen, [](const UiNode& n) { return n.reveal; });
  REQUIRE(reveal != nullptr);
  CHECK(reveal->text == "two {i}there{/i}");
  CHECK(reveal->markup);
  CHECK(count_nodes(screen, [](const UiNode& n) { return n.kind == NodeKind::kText; }) == 3);
  const UiNode* name = find_node(screen, [](const UiNode& n) { return n.text == "Eileen"; });
  REQUIRE(name != nullptr);
  CHECK(name->style.color == vnmaki::rgba8(kLilacRed, kLilacGreen, kLilacBlue));

  // Not typing (a menu): no node reveals, and the screen is built in the same way.
  REQUIRE(game.runtime.build_screen("nvl", vnmaki::make_nvl_args(page, false), tree).empty());
  UiNode still = vnmaki::build_ui_tree(tree);
  CHECK(find_node(still, [](const UiNode& n) { return n.reveal; }) == nullptr);
}

TEST_CASE("nvl screen: a menu in NVL mode shows its options inside the NVL screen") {
  Game game{"nvl_menu", R"(
label("start", function()
  nvl(true)
  narrate("Pick.")
  store.choice = menu { "red", "blue" }
  narrate("picked " .. store.choice)
end)
)"};
  game.runtime.advance();
  REQUIRE(game.at() == "menu");
  game.screens.sync();
  REQUIRE(game.host.is_shown(std::string{vnmaki::kMenuScreenName}));
  UiNode* screen = game.host.find(vnmaki::kMenuScreenName);
  REQUIRE(find_node(*screen, [](const UiNode& n) { return n.text == "Pick."; }) != nullptr);
  const UiNode* blue = find_node(*screen, [](const UiNode& n) { return n.text == "blue"; });
  REQUIRE(blue != nullptr);
  CHECK(std::get<std::int64_t>(*blue->returns) == 2);  // the number the menu gives back

  game.click_on(*blue);
  CHECK(game.at() == "picked 2");
  CHECK_FALSE(game.host.is_shown(std::string{vnmaki::kMenuScreenName}));
}

// --- Unlocks and the gallery -------------------------------------------------------------------

constexpr const char* kUnlockStory = R"(
label("start", function()
  narrate(tostring(unlocked("cg", "cg/beach")) .. " " .. tostring(unlocked("cg", "cg/night")))
  show("hero")
  scene("cg/beach")
  narrate(tostring(unlocked("cg", "cg/beach")) .. " " .. tostring(unlocked("cg", "cg/night")))
  show("x", { image = "cg/night" })
  narrate(tostring(unlocked("cg", "cg/beach")) .. " " .. tostring(unlocked("cg", "cg/night")))
  unlock("cg", "custom")
  narrate(tostring(unlocked("cg", "custom")))
end)
)";

TEST_CASE("unlock: scene and show unlock the images that project.toml lists") {
  Game game{"unlock_show", kUnlockStory, {"cg/beach", "cg/night", "cg/day"}};
  CHECK(game.at() == "false false");
  game.runtime.advance();
  CHECK(game.at() == "true false");  // `hero` is not in the gallery; scene("cg/beach") is
  game.runtime.advance();
  CHECK(game.at() == "true true");  // show with `image` unlocks that image
  game.runtime.advance();
  CHECK(game.at() == "true");  // unlock(kind, name) by hand

  // Rollback never revokes: the first line is computed again and sees what persists.
  while (game.runtime.roll_back()) {
  }
  CHECK(game.at() == "true true");
}

TEST_CASE("unlock: a game with no gallery list unlocks nothing by show") {
  Game game{"unlock_none", kUnlockStory};
  game.runtime.advance();
  game.runtime.advance();
  CHECK(game.at() == "false false");
}

TEST_CASE("gallery screen: a grid with the unlocked images and a placeholder for the others") {
  Game game{"gallery_grid", kUnlockStory, {"cg/beach", "cg/night", "cg/day"}};
  game.runtime.advance();
  game.runtime.advance();  // cg/beach and cg/night are unlocked now
  REQUIRE(game.actions.open("gallery", {}, true).empty());

  UiNode* screen = game.host.find("gallery");
  REQUIRE(screen != nullptr);
  CHECK(count_nodes(*screen, [](const UiNode& n) { return n.kind == NodeKind::kImage; }) ==
        2);  // one picture for each unlocked image
  const auto is_button = [](const UiNode& n) { return n.kind == NodeKind::kButton; };
  CHECK(count_nodes(*screen, is_button) == kGalleryImages + 1);  // the cells and Back
  CHECK(count_nodes(*screen, [](const UiNode& n) {
          return n.kind == NodeKind::kButton && n.label == "Locked" && n.state.disabled;
        }) == 1);  // cg/day
  const UiNode* beach = find_node(*screen, [](const UiNode& n) { return n.label == "cg/beach"; });
  REQUIRE(beach != nullptr);
  CHECK(std::get<std::string>(*beach->returns) == "@view:cg/beach");
  CHECK_FALSE(beach->state.disabled);
}

TEST_CASE("gallery screen: mouse, keyboard and gamepad open an image and close it again") {
  Game game{"gallery_input", kUnlockStory, {"cg/beach", "cg/night", "cg/day"}};
  game.runtime.advance();
  game.runtime.advance();
  REQUIRE(game.actions.open("gallery", {}, true).empty());

  const auto viewed = [&game]() -> std::string {  // the image of the view, "" if none is shown
    UiNode* view = game.host.find("gallery_view");
    const UiNode* picture =
        view == nullptr ? nullptr
                        : find_node(*view, [](const UiNode& n) { return n.kind == NodeKind::kImage; });
    return picture == nullptr ? std::string{} : picture->text;
  };

  // Keyboard: the first enabled cell has the focus; Right moves to the next, Enter opens, Esc
  // closes.
  game.press(vnmaki::Key::kEnter);
  CHECK(viewed() == "cg/beach");
  game.press(vnmaki::Key::kEscape);
  CHECK(viewed().empty());
  game.press(vnmaki::Key::kRight);
  game.press(vnmaki::Key::kEnter);
  CHECK(viewed() == "cg/night");
  game.press(vnmaki::Key::kEscape);
  CHECK(game.host.is_shown("gallery"));

  // Gamepad: the d-pad moves (the locked cell cannot be reached), south opens, east closes.
  game.tap(vnmaki::GamepadButton::kDpadLeft);
  game.tap(vnmaki::GamepadButton::kSouth);
  CHECK(viewed() == "cg/beach");
  game.tap(vnmaki::GamepadButton::kEast);
  CHECK(viewed().empty());

  // Mouse: a click on the cell opens the view, a click on the picture closes it.
  game.click_on(*find_node(*game.host.find("gallery"),
                           [](const UiNode& n) { return n.label == "cg/night"; }));
  CHECK(viewed() == "cg/night");
  game.click_on(*find_node(*game.host.find("gallery_view"),
                           [](const UiNode& n) { return n.kind == NodeKind::kButton; }));
  CHECK(viewed().empty());
  CHECK(game.host.is_shown("gallery"));

  game.tap(vnmaki::GamepadButton::kEast);  // east on the gallery itself: @close
  CHECK_FALSE(game.host.is_shown("gallery"));
  CHECK(game.warnings.empty());
}

TEST_CASE("gallery screen: the title screen offers it only when the game has a gallery") {
  Game with{"title_with", kUnlockStory, {"cg/beach"}};
  Game without{"title_without", kUnlockStory};
  vnmaki::ScreenNodeDef args{.kind = "args", .props = {{"title", std::string{"T"}}}};
  vnmaki::ScreenNodeDef tree;
  REQUIRE(with.runtime.build_screen("title", args, tree).empty());
  CHECK(has_text(tree, "Gallery"));
  REQUIRE(without.runtime.build_screen("title", args, tree).empty());
  CHECK_FALSE(has_text(tree, "Gallery"));

  // With no images the gallery says so, and still has a way back.
  REQUIRE(without.actions.open("gallery", {}, true).empty());
  CHECK(find_node(*without.host.find("gallery"),
                  [](const UiNode& n) { return n.text == "This game has no gallery."; }) != nullptr);
}
