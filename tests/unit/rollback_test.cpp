// Rollback (docs/SPEC.md section 8) through ScriptRuntime::roll_back(): in a label, across labels
// and calls, across a menu, at the depth limit, over waits, and the headless RollbackInput.
// Expected values are written out by hand from the script text.

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/dialogue/rollback_input.hpp"
#include "engine/platform/input.hpp"
#include "engine/runtime/headless_runner.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::AdvanceInput;
using vnmaki::ChooseInput;
using vnmaki::HeadlessOutcome;
using vnmaki::RollbackInput;
using vnmaki::RunState;
using vnmaki::ScriptInput;
using vnmaki::ScriptRuntime;
using Lines = std::vector<std::string>;

constexpr std::size_t kSmallDepth = 3;
constexpr std::size_t kLineCount = 8;
constexpr float kWheelNotch = 1;

// A story over one script that waits at its start label. `at()` names what it waits for.
struct Story {
  explicit Story(const std::string& source, std::size_t depth = vnmaki::kDefaultRollbackDepth)
      : vfs(vnmaki::test::memory_vfs({{"scripts/main.lua", source}})),
        runtime(vfs, {.rollback_depth = depth}) {
    runtime.load_scripts();
    runtime.start("start");
  }

  // The text of a narrate line, "menu", "wait" or "screen <name>".
  [[nodiscard]] std::string at() const {
    const vnmaki::BlockingRequest* request = runtime.request();
    if (request == nullptr) {
      return "no request";
    }
    if (const auto* narrate = std::get_if<vnmaki::NarrateRequest>(request)) {
      return narrate->text;
    }
    if (std::holds_alternative<vnmaki::MenuRequest>(*request)) {
      return "menu";
    }
    if (std::holds_alternative<vnmaki::WaitRequest>(*request)) {
      return "wait";
    }
    if (const auto* screen = std::get_if<vnmaki::CallScreenRequest>(request)) {
      return "screen " + screen->name;
    }
    return "other";
  }

  // Goes back as often as it can; returns the texts it passed, the first one included.
  Lines roll_back_all() {
    Lines seen{at()};
    while (runtime.roll_back()) {
      seen.push_back(at());
    }
    return seen;
  }

  vnmaki::Vfs vfs;
  ScriptRuntime runtime;
};

// Counts up in `store`, so a rollback that does not restore the store shows in the text.
constexpr auto kThreeLines = R"(
label("start", function()
  store.n = (store.n or 0) + 1
  narrate("line " .. store.n)
  store.n = store.n + 1
  narrate("line " .. store.n)
  store.n = store.n + 1
  narrate("line " .. store.n)
end)
)";

}  // namespace

TEST_CASE("rollback: in a label it goes back one line at a time and restores the store") {
  Story story{kThreeLines};
  story.runtime.advance();
  story.runtime.advance();
  REQUIRE(story.at() == "line 3");

  CHECK(story.runtime.roll_back());
  CHECK(story.at() == "line 2");
  CHECK(story.runtime.roll_back());
  CHECK(story.at() == "line 1");
  CHECK(story.runtime.position() == vnmaki::ScriptPosition{.label = "start", .step = 1});
  CHECK(story.runtime.rollback_steps().size() == 1);

  SUBCASE("there is nothing before the first line") {
    CHECK_FALSE(story.runtime.can_roll_back());
    CHECK_FALSE(story.runtime.roll_back());
    CHECK(story.at() == "line 1");
  }
  SUBCASE("the story goes forward again, and counts again from the restored store") {
    story.runtime.advance();
    CHECK(story.at() == "line 2");
    story.runtime.advance();
    CHECK(story.at() == "line 3");
    CHECK(story.runtime.state() == RunState::kWaiting);
  }
}

TEST_CASE("rollback: a story that has ended or never started has nowhere to go back to") {
  Story story{kThreeLines};
  CHECK_FALSE(story.runtime.can_roll_back());  // the first line only
  story.runtime.advance();
  story.runtime.advance();
  story.runtime.advance();
  REQUIRE(story.runtime.state() == RunState::kFinished);
  CHECK_FALSE(story.runtime.roll_back());
}

TEST_CASE("rollback: persistent does not change") {
  Story story{R"(
label("start", function()
  narrate("a")
  persistent.reached_b = true
  narrate("b")
end)
)"};
  story.runtime.advance();
  const vnmaki::StoreSnapshot before = story.runtime.persistent_snapshot();

  REQUIRE(story.runtime.roll_back());

  CHECK(story.at() == "a");
  CHECK(story.runtime.persistent_snapshot() == before);
}

TEST_CASE("rollback: across a jump it replays the label that was running before") {
  Story story{R"(
label("start", function()
  store.visited = (store.visited or 0) + 1
  narrate("start " .. store.visited)
  jump("next")
end)
label("next", function()
  store.visited = store.visited + 1
  narrate("next " .. store.visited)
  narrate("next again")
end)
)"};
  story.runtime.advance();
  REQUIRE(story.at() == "next 2");
  REQUIRE(story.runtime.position().label == "next");

  REQUIRE(story.runtime.roll_back());
  CHECK(story.at() == "start 1");
  CHECK(story.runtime.position() == vnmaki::ScriptPosition{.label = "start", .step = 1});

  story.runtime.advance();
  CHECK(story.at() == "next 2");
  story.runtime.advance();
  CHECK(story.at() == "next again");
  CHECK(story.roll_back_all() == Lines{"next again", "next 2", "start 1"});
}

TEST_CASE("rollback: across a call it rebuilds the callers, and ret still comes back") {
  Story story{R"(
label("start", function()
  narrate("main one")
  call("sub")
  narrate("main two")
end)
label("sub", function()
  narrate("sub one")
  narrate("sub two")
end)
)"};
  story.runtime.advance();
  story.runtime.advance();
  story.runtime.advance();
  REQUIRE(story.at() == "main two");

  SUBCASE("back into the called label") {
    REQUIRE(story.runtime.roll_back());
    CHECK(story.at() == "sub two");
    CHECK(story.runtime.position() == vnmaki::ScriptPosition{.label = "sub", .step = 2});
    CHECK(story.runtime.rollback_steps().back().stack.size() == 2);

    story.runtime.advance();
    CHECK(story.at() == "main two");  // the caller was rebuilt, so sub returns to it
  }
  SUBCASE("all the way to the start, and forward again") {
    CHECK(story.roll_back_all() == Lines{"main two", "sub two", "sub one", "main one"});
    story.runtime.advance();
    story.runtime.advance();
    story.runtime.advance();
    CHECK(story.at() == "main two");
  }
}

TEST_CASE("rollback: the same call twice goes back to the right one") {
  Story story{R"(
label("start", function()
  call("sub")
  call("sub")
  narrate("end")
end)
label("sub", function()
  store.calls = (store.calls or 0) + 1
  narrate("sub " .. store.calls)
end)
)"};
  story.runtime.advance();
  story.runtime.advance();
  REQUIRE(story.at() == "end");

  CHECK(story.roll_back_all() == Lines{"end", "sub 2", "sub 1"});
}

TEST_CASE("rollback: a label that jumps inside a call can be rolled back through") {
  Story story{R"(
label("start", function()
  narrate("main")
  call("first")
  narrate("after")
end)
label("first", function()
  narrate("first")
  jump("second")
end)
label("second", function()
  narrate("second")
end)
)"};
  story.runtime.advance();
  story.runtime.advance();
  story.runtime.advance();
  REQUIRE(story.at() == "after");

  CHECK(story.roll_back_all() == Lines{"after", "second", "first", "main"});
  story.runtime.advance();
  story.runtime.advance();
  story.runtime.advance();
  CHECK(story.at() == "after");
}

TEST_CASE("rollback: at a menu the player can choose again, and the old branch is gone") {
  Story story{R"(
label("start", function()
  narrate("intro")
  local c = menu({ "a", "b" })
  narrate("got " .. c)
  narrate("tail")
end)
)"};
  story.runtime.advance();
  REQUIRE(story.at() == "menu");
  story.runtime.choose(0);
  story.runtime.advance();
  REQUIRE(story.at() == "tail");

  REQUIRE(story.runtime.roll_back());
  CHECK(story.at() == "got 1");
  REQUIRE(story.runtime.roll_back());
  CHECK(story.at() == "menu");
  CHECK(story.runtime.rollback_steps().size() == 2);

  story.runtime.choose(1);
  CHECK(story.at() == "got 2");
  CHECK(story.runtime.rollback_steps().size() == 3);  // intro, menu, got 2: nothing of "got 1"
  story.runtime.advance();
  CHECK(story.at() == "tail");
  CHECK(story.roll_back_all() == Lines{"tail", "got 2", "menu", "intro"});
}

TEST_CASE("rollback: a menu in a called label keeps its answer for the log of the caller") {
  Story story{R"(
label("start", function()
  call("ask")
  narrate("back")
end)
label("ask", function()
  local c = menu({ "a", "b" })
  narrate("c" .. c)
end)
)"};
  story.runtime.choose(1);
  story.runtime.advance();
  REQUIRE(story.at() == "back");

  CHECK(story.roll_back_all() == Lines{"back", "c2", "menu"});
  story.runtime.choose(0);
  CHECK(story.at() == "c1");
  story.runtime.advance();
  CHECK(story.at() == "back");
}

TEST_CASE("rollback: a fixed menu cannot be chosen again") {
  Story story{R"(
label("start", function()
  narrate("intro")
  local c = menu({ "a", "b", fixed = true })
  narrate("got " .. c)
  narrate("tail")
end)
)"};
  story.runtime.advance();
  REQUIRE(story.at() == "menu");

  SUBCASE("before the answer the player can still go back to the line before") {
    CHECK(story.runtime.roll_back());
    CHECK(story.at() == "intro");
    story.runtime.advance();
    CHECK(story.at() == "menu");
  }
  SUBCASE("after the answer rollback stops at the menu") {
    story.runtime.choose(1);
    CHECK(story.at() == "got 2");
    story.runtime.advance();
    CHECK(story.at() == "tail");

    CHECK(story.runtime.roll_back());
    CHECK(story.at() == "got 2");
    CHECK_FALSE(story.runtime.can_roll_back());
    CHECK_FALSE(story.runtime.roll_back());
  }
}

TEST_CASE("rollback: the fixed option must be a boolean") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({{"scripts/main.lua", R"(
label("start", function()
  menu({ "a", fixed = "yes" })
end)
)"}});
  ScriptRuntime runtime{vfs};
  runtime.load_scripts();
  runtime.start("start");

  CHECK(runtime.state() == RunState::kFailed);
  CHECK(runtime.error().find("bad option 'fixed' to 'menu' (boolean expected, got string)") !=
        std::string::npos);
}

TEST_CASE("rollback: the depth limit forgets the oldest lines") {
  std::string script = "label(\"start\", function()\n";
  for (std::size_t line = 1; line <= kLineCount; ++line) {
    script += "  narrate(\"l" + std::to_string(line) + "\")\n";
  }
  script += "end)\n";
  Story story{script, kSmallDepth};
  for (std::size_t line = 1; line < kLineCount; ++line) {
    story.runtime.advance();
  }
  REQUIRE(story.at() == "l8");
  CHECK(story.runtime.rollback_steps().size() == kSmallDepth + 1);

  CHECK(story.roll_back_all() == Lines{"l8", "l7", "l6", "l5"});
  CHECK(story.runtime.rollback_steps().size() == 1);
  story.runtime.advance();
  CHECK(story.at() == "l6");
}

TEST_CASE("rollback: depth 0 turns it off") {
  Story story{kThreeLines, 0};
  story.runtime.advance();
  CHECK_FALSE(story.runtime.can_roll_back());
  CHECK_FALSE(story.runtime.roll_back());
  CHECK(story.at() == "line 2");
}

TEST_CASE("rollback: it goes back over a wait to the previous line") {
  Story story{R"(
label("start", function()
  narrate("before")
  wait_all()
  narrate("after")
end)
)"};
  story.runtime.advance();
  REQUIRE(story.at() == "wait");
  REQUIRE(story.runtime.position().wait);

  SUBCASE("from the wait") {
    CHECK(story.runtime.roll_back());
    CHECK(story.at() == "before");
    CHECK_FALSE(story.runtime.position().wait);
  }
  SUBCASE("from the line after it") {
    story.runtime.advance();
    REQUIRE(story.at() == "after");
    CHECK(story.runtime.position() == vnmaki::ScriptPosition{.label = "start", .step = 3});

    CHECK(story.runtime.roll_back());
    CHECK(story.at() == "before");  // not the wait in between
    CHECK(story.runtime.position() == vnmaki::ScriptPosition{.label = "start", .step = 1});
    CHECK_FALSE(story.runtime.can_roll_back());
    story.runtime.advance();
    CHECK(story.at() == "wait");
  }
}

TEST_CASE("rollback: a wait before the first line has nothing to go back to") {
  Story story{R"(
label("start", function()
  wait_all()
  narrate("first")
end)
)"};
  REQUIRE(story.at() == "wait");
  CHECK_FALSE(story.runtime.can_roll_back());
  story.runtime.advance();
  CHECK(story.at() == "first");
  CHECK_FALSE(story.runtime.can_roll_back());
}

TEST_CASE("rollback: a load starts the history again") {
  Story story{kThreeLines};
  story.runtime.advance();
  story.runtime.advance();
  REQUIRE(story.runtime.rollback_steps().size() == 3);
  const vnmaki::LabelSnapshot snapshot = *story.runtime.label_snapshot();
  const std::vector<vnmaki::InputResult> inputs{story.runtime.input_log().begin(),
                                                story.runtime.input_log().end()};

  story.runtime.replay(snapshot, inputs, 2);

  CHECK(story.at() == "line 2");
  CHECK(story.runtime.rollback_steps().size() == 1);
  CHECK_FALSE(story.runtime.can_roll_back());
}

TEST_CASE("rollback input: the wheel up, Page Up and the left shoulder button") {
  using vnmaki::is_rollback_event;
  CHECK(is_rollback_event(vnmaki::WheelEvent{.dy = kWheelNotch}));
  CHECK_FALSE(is_rollback_event(vnmaki::WheelEvent{.dy = -kWheelNotch}));
  CHECK(is_rollback_event(vnmaki::KeyEvent{.key = vnmaki::Key::kPageUp, .pressed = true}));
  CHECK_FALSE(is_rollback_event(vnmaki::KeyEvent{.key = vnmaki::Key::kPageUp, .pressed = false}));
  CHECK_FALSE(is_rollback_event(
      vnmaki::KeyEvent{.key = vnmaki::Key::kPageUp, .pressed = true, .repeat = true}));
  CHECK_FALSE(is_rollback_event(vnmaki::KeyEvent{.key = vnmaki::Key::kEnter, .pressed = true}));
  CHECK(is_rollback_event(
      vnmaki::GamepadButtonEvent{.button = vnmaki::GamepadButton::kLeftShoulder, .pressed = true}));
  CHECK_FALSE(is_rollback_event(
      vnmaki::GamepadButtonEvent{.button = vnmaki::GamepadButton::kSouth, .pressed = true}));
  CHECK_FALSE(is_rollback_event(vnmaki::QuitRequestedEvent{}));
}

TEST_CASE("headless: a rollback input shows the line it went back to") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({{"scripts/main.lua", R"(
label("start", function()
  narrate("one")
  local c = menu({ "a", "b" })
  narrate("got " .. c)
  wait_all()
  narrate("end")
end)
)"}});

  SUBCASE("lines, a menu to choose again, and a wait that is gone back over") {
    const std::vector<ScriptInput> inputs{
        AdvanceInput{},  ChooseInput{.index = 0}, AdvanceInput{}, RollbackInput{},
        RollbackInput{}, ChooseInput{.index = 1}, AdvanceInput{}, AdvanceInput{}};
    const vnmaki::HeadlessResult result = vnmaki::run_headless(vfs, inputs);

    CHECK(result.outcome == HeadlessOutcome::kFinished);
    CHECK(result.transcript == Lines{"label start", "narrate one", "menu 1: a", "menu 2: b",
                                     "narrate got 1", "wait_all", "narrate end",
                                     "narrate got 1",  // rollback from "end": over the wait
                                     "menu 1: a", "menu 2: b",  // rollback
                                     "narrate got 2", "wait_all", "narrate end"});
  }
  SUBCASE("a rollback with nothing before it fails the run") {
    const std::vector<ScriptInput> inputs{RollbackInput{}};
    const vnmaki::HeadlessResult result = vnmaki::run_headless(vfs, inputs);

    CHECK(result.outcome == HeadlessOutcome::kFailed);
    CHECK(result.error == "the next input is a rollback, but there is nothing to go back to");
  }
}
