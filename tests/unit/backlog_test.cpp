// The backlog (docs/SPEC.md section 8): its entries after play, rollback and load, voice replay
// through Audio, and the scrolling screen. Expected values are written out by hand from the
// script text.

#include "engine/dialogue/backlog.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/audio/audio.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/viewport.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/ui/backlog_screen.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/ui/ui_node.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/audio_fixtures.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::Backlog;
using vnmaki::BacklogEntry;
using vnmaki::BacklogScreen;
using vnmaki::Key;
using Texts = std::vector<std::string>;

constexpr vnmaki::Size kCanvas{.width = 1280, .height = 720};
constexpr std::size_t kLongStory = 40;
constexpr std::size_t kKeptEntries = 2;
constexpr std::size_t kSmallDepth = 3;
constexpr double kClip = 0.5;  // longer than the voice fixture, in seconds

const vnmaki::Viewport kViewport =
    vnmaki::compute_viewport(kCanvas, kCanvas, vnmaki::ScaleMode::kFit);

constexpr auto kStory = R"(
label("start", function()
  voice("eileen/001")
  say("Eileen", "Hello.")
  narrate("Silence.")
  menu { "a", "b" }
  say("Eileen", "After.")
end)
)";

// A long story: say lines "Line 1" ... "Line 40"; the first has a voice.
constexpr auto kLines = R"(
label("start", function()
  voice("eileen/001")
  for i = 1, 40 do
    say("Eileen", "Line " .. i)
  end
end)
)";

struct Story {
  explicit Story(const char* script, std::size_t depth = vnmaki::kDefaultRollbackDepth)
      : vfs(vnmaki::test::memory_vfs(
            {{"scripts/main.lua", script},
             {"audio/voice/eileen/001.opus", vnmaki::test::audio_fixture("tone.opus")}})),
        audio(vfs),
        runtime(vfs, {.rollback_depth = depth, .scene_state = &audio, .audio = &audio}) {
    runtime.load_scripts();
    runtime.start("start");
  }

  // The texts of the entries, oldest first.
  [[nodiscard]] Texts texts(const Backlog& backlog) const {
    Texts result;
    for (const BacklogEntry& entry : backlog.entries()) {
      result.push_back(entry.text);
    }
    return result;
  }

  vnmaki::Vfs vfs;
  vnmaki::Audio audio;
  vnmaki::ScriptRuntime runtime;
};

vnmaki::KeyEvent press(Key key) { return {.key = key, .pressed = true}; }

vnmaki::GamepadButtonEvent pad(vnmaki::GamepadButton button) {
  return {.button = button, .pressed = true};
}

// A story in the middle of kLines with the backlog open.
struct Screen {
  Screen() : story(kLines), backlog(story.runtime, &story.audio), screen(host, backlog) {
    for (std::size_t i = 1; i < kLongStory; ++i) {
      story.runtime.advance();
    }
    screen.open();
  }

  [[nodiscard]] bool send(const vnmaki::InputEvent& event) {
    return screen.handle_event(event, kViewport);
  }

  [[nodiscard]] Texts shown() {
    Texts result;
    const vnmaki::UiNode* root = host.find(vnmaki::kBacklogScreenName);
    REQUIRE(root != nullptr);
    for (const vnmaki::UiNode& entry : root->children.back().children) {
      result.push_back(entry.children.back().text);
    }
    return result;
  }

  Story story;
  vnmaki::UiHost host{kCanvas};
  Backlog backlog;
  BacklogScreen screen;
};

}  // namespace

TEST_CASE("backlog: entries hold speaker, text, label, step and voice; menus are left out") {
  Story story{kStory};
  const Backlog backlog{story.runtime, &story.audio};
  story.runtime.advance();
  story.runtime.advance();  // past the narrate, at the menu
  story.runtime.choose(0);

  const std::vector<BacklogEntry> entries = backlog.entries();
  REQUIRE(entries.size() == 3);
  CHECK(entries[0] == BacklogEntry{.speaker = "Eileen",
                                   .text = "Hello.",
                                   .label = "start",
                                   .step = 1,
                                   .voice = "eileen/001"});
  CHECK(entries[1] == BacklogEntry{.text = "Silence.", .label = "start", .step = 2});
  CHECK(entries[2] ==
        BacklogEntry{.speaker = "Eileen", .text = "After.", .label = "start", .step = 4});
}

TEST_CASE("backlog: a rollback takes the lines after the one it goes back to") {
  Story story{kStory};
  const Backlog backlog{story.runtime, &story.audio};
  story.runtime.advance();
  story.runtime.advance();
  story.runtime.choose(1);
  REQUIRE(story.texts(backlog) == Texts{"Hello.", "Silence.", "After."});

  REQUIRE(story.runtime.roll_back());  // back at the menu
  CHECK(story.texts(backlog) == Texts{"Hello.", "Silence."});
  REQUIRE(story.runtime.roll_back());
  REQUIRE(story.runtime.roll_back());
  CHECK(story.texts(backlog) == Texts{"Hello."});
  CHECK(backlog.entries()[0].voice == "eileen/001");  // the voice survives the replay

  story.runtime.advance();  // forward again: a new branch with no trace of the old one
  CHECK(story.texts(backlog) == Texts{"Hello.", "Silence."});
}

TEST_CASE("backlog: after a load the log starts again, with the voice of the line") {
  Story story{kStory};
  const Backlog backlog{story.runtime, &story.audio};
  const vnmaki::LabelSnapshot entry = *story.runtime.label_snapshot();
  story.runtime.advance();
  story.runtime.advance();

  story.runtime.replay(entry, {}, 1);
  REQUIRE(story.runtime.state() == vnmaki::RunState::kWaiting);
  const std::vector<BacklogEntry> entries = backlog.entries();
  REQUIRE(entries.size() == 1);
  CHECK(entries[0].text == "Hello.");
  CHECK(entries[0].voice == "eileen/001");

  story.runtime.replay(entry, {}, 2);
  REQUIRE(backlog.entries().size() == 1);
  CHECK(backlog.entries()[0].text == "Silence.");
  CHECK(backlog.entries()[0].voice.empty());  // the voice belonged to the line before
}

TEST_CASE("backlog: the length is capped, and the rollback depth caps it too") {
  Story story{kLines};
  for (std::size_t i = 1; i < kLongStory; ++i) {
    story.runtime.advance();
  }
  const Backlog capped{story.runtime, &story.audio, kKeptEntries};
  CHECK(story.texts(capped) == Texts{"Line 39", "Line 40"});

  Story shallow{kLines, kSmallDepth};
  for (std::size_t i = 1; i < kLongStory; ++i) {
    shallow.runtime.advance();
  }
  const Backlog backlog{shallow.runtime, &shallow.audio};
  CHECK(backlog.entries().size() == kSmallDepth + 1);  // the current line and 3 before it
}

TEST_CASE("backlog: playing an entry plays its voice again through Audio") {
  Story story{kStory};
  const Backlog backlog{story.runtime, &story.audio};
  story.audio.update(kClip);  // the live voice has ended
  REQUIRE_FALSE(story.audio.voice_playing());
  const std::vector<BacklogEntry> entries = backlog.entries();
  REQUIRE(entries.size() == 1);

  CHECK(backlog.play_voice(entries[0]).empty());

  CHECK(story.audio.voice_playing());
  CHECK(story.audio.channel("voice").tracks.at(0).name == "voice/eileen/001");
  story.audio.update(kClip);
  CHECK_FALSE(story.audio.voice_playing());
  CHECK(backlog.play_voice(BacklogEntry{.text = "No voice."}).empty());
  CHECK_FALSE(story.audio.voice_playing());
}

TEST_CASE("backlog screen: opens on the newest entry and shows the lines that fit") {
  Screen game;

  CHECK(game.screen.is_open());
  REQUIRE(game.screen.entries().size() == kLongStory);
  CHECK(game.screen.selected() == kLongStory - 1);
  const Texts shown = game.shown();
  CHECK(shown.size() == game.screen.visible_count());
  CHECK(shown.size() > 1);
  CHECK(shown.size() < kLongStory);
  CHECK(shown.back() == "Line 40");
  CHECK(game.screen.first_visible() == kLongStory - shown.size());
}

TEST_CASE("backlog screen: keys, wheel and gamepad scroll it, and it keeps the selection shown") {
  Screen game;
  const std::size_t page = game.screen.visible_count();

  CHECK(game.send(press(Key::kUp)));
  CHECK(game.screen.selected() == kLongStory - 2);
  CHECK(game.send(vnmaki::WheelEvent{.dy = 1}));
  CHECK(game.screen.selected() == kLongStory - 3);
  CHECK(game.send(pad(vnmaki::GamepadButton::kDpadDown)));
  CHECK(game.screen.selected() == kLongStory - 2);

  CHECK(game.send(press(Key::kHome)));
  CHECK(game.screen.selected() == 0);
  CHECK(game.screen.first_visible() == 0);
  CHECK(game.shown().front() == "Line 1");
  CHECK(game.send(press(Key::kUp)));  // stays at the oldest
  CHECK(game.screen.selected() == 0);

  CHECK(game.send(press(Key::kPageDown)));
  CHECK(game.screen.selected() == page);
  CHECK(game.send(pad(vnmaki::GamepadButton::kLeftShoulder)));
  CHECK(game.screen.selected() == 0);

  for (std::size_t i = 0; i < page; ++i) {
    CHECK(game.send(press(Key::kDown)));
  }
  CHECK(game.screen.selected() == page);
  CHECK(game.screen.first_visible() == 1);  // the window moved by one entry to keep it shown
  CHECK(game.send(press(Key::kEnd)));
  CHECK(game.screen.selected() == kLongStory - 1);
  CHECK(game.shown().back() == "Line 40");
}

TEST_CASE("backlog screen: Enter, the south button and a click play the voice of an entry") {
  Screen game;
  game.story.audio.update(kClip);
  REQUIRE(game.send(press(Key::kHome)));  // "Line 1" is the line with the voice
  REQUIRE_FALSE(game.story.audio.voice_playing());

  CHECK(game.send(press(Key::kEnter)));
  CHECK(game.story.audio.voice_playing());

  game.story.audio.update(kClip);
  CHECK(game.send(pad(vnmaki::GamepadButton::kSouth)));
  CHECK(game.story.audio.voice_playing());

  game.story.audio.update(kClip);
  const vnmaki::UiNode* root = game.host.find(vnmaki::kBacklogScreenName);
  REQUIRE(root != nullptr);
  const vnmaki::UiNode& entry = root->children.back().children.front();
  REQUIRE(entry.kind == vnmaki::NodeKind::kButton);  // only a line with a voice is a button
  const vnmaki::PointF centre{.x = entry.rect.x + (entry.rect.width / 2),
                              .y = entry.rect.y + (entry.rect.height / 2)};
  CHECK(game.send(vnmaki::PointerButtonEvent{
      .button = vnmaki::MouseButton::kLeft, .pressed = true, .position = centre}));
  CHECK(game.story.audio.voice_playing());

  CHECK(game.send(press(Key::kDown)));  // "Line 2" has no voice
  game.story.audio.update(kClip);
  CHECK(game.send(press(Key::kEnter)));
  CHECK_FALSE(game.story.audio.voice_playing());
}

TEST_CASE("backlog screen: L and the north button open it, Esc, L and the east button close it") {
  Story story{kLines};
  vnmaki::UiHost host{kCanvas};
  const Backlog backlog{story.runtime, &story.audio};
  BacklogScreen screen{host, backlog};

  CHECK_FALSE(screen.handle_event(vnmaki::KeyEvent{.key = Key::kA, .pressed = true}, kViewport));
  CHECK_FALSE(host.is_shown(vnmaki::kBacklogScreenName));
  CHECK(screen.handle_event(press(Key::kL), kViewport));
  CHECK(host.is_shown(vnmaki::kBacklogScreenName));
  CHECK(screen.handle_event(press(Key::kEscape), kViewport));
  CHECK_FALSE(screen.is_open());
  CHECK_FALSE(host.is_shown(vnmaki::kBacklogScreenName));

  CHECK(screen.handle_event(pad(vnmaki::GamepadButton::kNorth), kViewport));
  CHECK(screen.is_open());
  // Every key belongs to the screen while it is open.
  CHECK(screen.handle_event(press(Key::kEnter), kViewport));
  CHECK(screen.handle_event(press(Key::kA), kViewport));
  CHECK(screen.handle_event(pad(vnmaki::GamepadButton::kEast), kViewport));
  CHECK_FALSE(screen.is_open());

  host.show("menu", vnmaki::make_node(vnmaki::NodeKind::kBox), true);
  CHECK_FALSE(screen.handle_event(press(Key::kL), kViewport));  // a modal screen has the keys
}
