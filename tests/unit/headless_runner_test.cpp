// The headless runner seam: scripts plus scripted inputs give a transcript.
// The expected transcripts are written out by hand from the script text.

#include "engine/runtime/headless_runner.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::HeadlessOutcome;
using vnmaki::HeadlessResult;
using vnmaki::ScriptInput;
using Lines = std::vector<std::string>;

std::vector<ScriptInput> advances(std::size_t count) {
  return std::vector<ScriptInput>(count, vnmaki::AdvanceInput{});
}

HeadlessResult run_script(const std::string& source, std::size_t advance_count = 0) {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({{"scripts/main.lua", source}});
  return vnmaki::run_headless(vfs, advances(advance_count));
}

}  // namespace

TEST_CASE("headless: the start label runs until its first say") {
  const HeadlessResult result = run_script(R"(
label("start", function()
  say("Eileen", "You came back.")
  say("Eileen", "Never reached without an advance.")
end)
)");

  CHECK(result.outcome == HeadlessOutcome::kOutOfInput);
  CHECK(result.transcript == Lines{"label start", "say Eileen: You came back."});
  CHECK(result.error.empty());
}

TEST_CASE("headless: each advance resumes the label at its next line") {
  const HeadlessResult result = run_script(R"(
label("start", function()
  narrate("It was raining.")
  say("Eileen", "You came back.")
  say(nil, "She smiled.")
end)
)",
                                           3);

  CHECK(result.outcome == HeadlessOutcome::kFinished);
  CHECK(result.transcript == Lines{"label start", "narrate It was raining.",
                                   "say Eileen: You came back.", "narrate She smiled."});
}

TEST_CASE("headless: a character table speaks by its name") {
  const HeadlessResult result = run_script(R"(
local eileen = { name = "Eileen" }
label("start", function() say(eileen, "Hi.") end)
)",
                                           1);

  CHECK(result.transcript == Lines{"label start", "say Eileen: Hi."});
}

TEST_CASE("headless: a multi-line scene of characters plays line by line to a clear end") {
  const std::string source = R"(
local eileen = character { name = "Eileen", color = "#c8a2c8", voice_tag = "eileen" }
local lucy = character { name = "Lucy" }
label("start", function()
  narrate("It was raining.")
  say(eileen, "You came back.")
  say(lucy, "Took you long enough.")
  say(eileen, "I know.")
end)
)";
  const Lines lines{"label start", "narrate It was raining.", "say Eileen: You came back.",
                    "say Lucy: Took you long enough.", "say Eileen: I know."};
  constexpr std::size_t kLineCount = 4;

  // One input per line reaches the end; one fewer stops at the last line.
  const HeadlessResult all = run_script(source, kLineCount);
  CHECK(all.outcome == HeadlessOutcome::kFinished);
  CHECK(all.transcript == lines);
  CHECK(all.stops.size() == kLineCount);

  const HeadlessResult short_of_end = run_script(source, kLineCount - 1);
  CHECK(short_of_end.outcome == HeadlessOutcome::kOutOfInput);
  CHECK(short_of_end.transcript == lines);
}

TEST_CASE("headless: inputs left over after the story ends are ignored") {
  const HeadlessResult result = run_script(R"(
label("start", function() narrate("Only line.") end)
)",
                                           5);

  CHECK(result.outcome == HeadlessOutcome::kFinished);
  CHECK(result.transcript == Lines{"label start", "narrate Only line."});
}

TEST_CASE("headless: jump leaves the label for another and never comes back") {
  const HeadlessResult result = run_script(R"(
label("start", function()
  narrate("before")
  jump("other")
  narrate("never said")
end)
label("other", function() narrate("in other") end)
)",
                                           2);

  CHECK(result.outcome == HeadlessOutcome::kFinished);
  CHECK(result.transcript ==
        Lines{"label start", "narrate before", "label other", "narrate in other"});
}

TEST_CASE("headless: call runs a label and ret comes back after the call") {
  const HeadlessResult result = run_script(R"(
label("start", function()
  call("aside")
  narrate("back in start")
end)
label("aside", function()
  narrate("in aside")
  ret()
  narrate("never said")
end)
)",
                                           2);

  CHECK(result.outcome == HeadlessOutcome::kFinished);
  CHECK(result.transcript == Lines{"label start", "label aside", "narrate in aside", "resume start",
                                   "narrate back in start"});
}

TEST_CASE("headless: a called label that runs off its end returns to the caller") {
  const HeadlessResult result = run_script(R"(
label("start", function()
  call("aside")
  call("aside")
end)
label("aside", function() narrate("aside") end)
)",
                                           2);

  CHECK(result.outcome == HeadlessOutcome::kFinished);
  CHECK(result.transcript == Lines{"label start", "label aside", "narrate aside", "resume start",
                                   "label aside", "narrate aside", "resume start"});
}

TEST_CASE("headless: each yield point is a step of the label that reached it") {
  const HeadlessResult result = run_script(R"(
label("start", function()
  narrate("one")
  narrate("two")
  call("aside")
  narrate("three")
  jump("next")
end)
label("aside", function() narrate("aside one") end)
label("next", function() narrate("next one") end)
)",
                                           5);

  using vnmaki::ScriptPosition;
  CHECK(result.outcome == HeadlessOutcome::kFinished);
  CHECK(result.stops == std::vector<ScriptPosition>{
                            {"start", 1}, {"start", 2}, {"aside", 1}, {"start", 3}, {"next", 1}});
}

TEST_CASE("headless: endless nested calls fail instead of growing the call stack forever") {
  const HeadlessResult result = run_script(R"(label("start", function() call("start") end))");

  CHECK(result.outcome == HeadlessOutcome::kFailed);
  CHECK(result.error == "label 'start': more than 1000 nested calls (a call without a return?)");
}

TEST_CASE("headless: ret from the outermost label ends the story") {
  const HeadlessResult result = run_script(R"(
label("start", function()
  ret()
  narrate("never said")
end)
)");

  CHECK(result.outcome == HeadlessOutcome::kFinished);
  CHECK(result.transcript == Lines{"label start"});
}

TEST_CASE("headless: labels in several scripts see each other") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({
      {"scripts/a.lua", R"(label("start", function() jump("chapter1") end))"},
      {"scripts/chapters/one.lua", R"(label("chapter1", function() narrate("one") end))"},
      {"scripts/notes.txt", "not a script"},
  });

  const HeadlessResult result = vnmaki::run_headless(vfs, advances(1));

  CHECK(result.outcome == HeadlessOutcome::kFinished);
  CHECK(result.transcript == Lines{"label start", "label chapter1", "narrate one"});
}

// The demo project from tools/generators/runtime_fixtures.py, read from disk.
TEST_CASE("headless: runs a project from a loose-file folder") {
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_FIXTURES_DIR} /
                                                 "runtime" / "demo")));

  const HeadlessResult result = vnmaki::run_headless(vfs, advances(4));

  CHECK(result.outcome == HeadlessOutcome::kFinished);
  CHECK(result.transcript == Lines{"label start", "narrate It was raining.",
                                   "say Eileen: You came back.", "label aside",
                                   "say Lucy: Took you long enough.", "resume start",
                                   "label ending", "narrate The end."});
}

TEST_CASE("headless: jumping to an unknown label fails naming the script and line") {
  const HeadlessResult result = run_script(R"(
label("start", function()
  jump("nowhere")
end)
)");

  CHECK(result.outcome == HeadlessOutcome::kFailed);
  CHECK(result.error == "scripts/main.lua:3: unknown label 'nowhere'");
}

TEST_CASE("headless: calling an unknown label fails naming the script and line") {
  const HeadlessResult result = run_script(R"(label("start", function()
  narrate("first")
  call("missing")
end)
)",
                                           1);

  CHECK(result.outcome == HeadlessOutcome::kFailed);
  CHECK(result.error == "scripts/main.lua:3: unknown label 'missing'");
}

TEST_CASE("headless: a missing start label fails with its name") {
  const HeadlessResult result = run_script(R"(label("begin", function() end))");

  CHECK(result.outcome == HeadlessOutcome::kFailed);
  CHECK(result.error == "unknown label 'start'");
}

TEST_CASE("headless: defining a label twice fails naming both places") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({
      {"scripts/a.lua", "label('start', function() end)"},
      {"scripts/b.lua", "\nlabel('start', function() end)"},
  });

  const HeadlessResult result = vnmaki::run_headless(vfs, {});

  CHECK(result.outcome == HeadlessOutcome::kFailed);
  CHECK(result.error == "scripts/b.lua:2: label 'start' is already defined at scripts/a.lua:1");
}

TEST_CASE("headless: a runtime error in a label fails naming the script and line") {
  const HeadlessResult result = run_script(R"(label("start", function()
  local missing = nil
  missing.field = 1
end)
)");

  CHECK(result.outcome == HeadlessOutcome::kFailed);
  CHECK(result.error.starts_with("scripts/main.lua:3: attempt to index"));
}

TEST_CASE("headless: a script that is not valid Lua fails naming the script and line") {
  const HeadlessResult result = run_script("label('start', function()\n  say('a' 'b')\nend)\n");

  CHECK(result.outcome == HeadlessOutcome::kFailed);
  CHECK(result.error.starts_with("scripts/main.lua:2: "));
}

TEST_CASE("headless: blocking calls outside a running label are errors") {
  SUBCASE("in top-level script code") {
    const HeadlessResult result = run_script("\nsay('Eileen', 'too early')\n");
    CHECK(result.outcome == HeadlessOutcome::kFailed);
    CHECK(result.error == "scripts/main.lua:2: say() can only be called from a running label");
  }
  SUBCASE("in a coroutine the script made") {
    const HeadlessResult result = run_script(R"(label("start", function()
  coroutine.wrap(function()
    narrate("from a script coroutine")
  end)()
end)
)");
    CHECK(result.outcome == HeadlessOutcome::kFailed);
    CHECK(result.error == "scripts/main.lua:3: narrate() can only be called from a running label");
  }
  SUBCASE("jump in top-level script code") {
    const HeadlessResult result = run_script("label('start', function() end)\njump('start')\n");
    CHECK(result.outcome == HeadlessOutcome::kFailed);
    CHECK(result.error == "scripts/main.lua:2: jump() can only be called from a running label");
  }
}

TEST_CASE("headless: labels can only be defined while scripts load") {
  const HeadlessResult result = run_script(R"(label("start", function()
  label("late", function() end)
end)
)");

  CHECK(result.outcome == HeadlessOutcome::kFailed);
  CHECK(result.error == "scripts/main.lua:2: label() can only be called while scripts load");
}

TEST_CASE("headless: a bare coroutine.yield in a label is an error, not a hang") {
  const HeadlessResult result = run_script(R"(label("start", function()
  coroutine.yield()
end)
)");

  CHECK(result.outcome == HeadlessOutcome::kFailed);
  CHECK(result.error ==
        "label 'start' yielded outside an engine call (coroutine.yield in a label?)");
}

TEST_CASE("headless: bad arguments to say are reported at the caller") {
  const HeadlessResult result = run_script(R"(label("start", function()
  say("Eileen", 42)
end)
)");

  CHECK(result.outcome == HeadlessOutcome::kFailed);
  CHECK(result.error ==
        "scripts/main.lua:2: bad argument #2 to 'say' (string expected, got number)");
}

TEST_CASE("headless: print goes to the transcript") {
  const HeadlessResult result =
      run_script(R"(label("start", function() print("score", 3, nil, true) end))");

  CHECK(result.outcome == HeadlessOutcome::kFinished);
  CHECK(result.transcript == Lines{"label start", "print score\t3\tnil\ttrue"});
}

TEST_CASE("headless: a menu lists its options and takes a choice") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({{"scripts/main.lua", R"(
label("start", function()
  local picked = menu({ "Of course I did.", "I forgot my bag." })
  narrate("picked " .. picked)
end)
)"}});
  const Lines until_the_menu{"label start", "menu 1: Of course I did.", "menu 2: I forgot my bag."};

  SUBCASE("a choice input picks the option") {
    const std::vector<ScriptInput> inputs{vnmaki::ChooseInput{.index = 1}, vnmaki::AdvanceInput{}};
    const HeadlessResult result = vnmaki::run_headless(vfs, inputs);

    CHECK(result.outcome == HeadlessOutcome::kFinished);
    Lines expected = until_the_menu;
    expected.emplace_back("narrate picked 2");
    CHECK(result.transcript == expected);
    CHECK(result.stops == std::vector<vnmaki::ScriptPosition>{{.label = "start", .step = 1},
                                                              {.label = "start", .step = 2}});
  }
  SUBCASE("an advance does not answer a menu") {
    const HeadlessResult result = vnmaki::run_headless(vfs, advances(1));

    CHECK(result.outcome == HeadlessOutcome::kFailed);
    CHECK(result.transcript == until_the_menu);
    CHECK(result.error ==
          "the story waits at a menu, and the next input is not one of its options");
  }
  SUBCASE("a choice the menu does not have is refused") {
    const std::vector<ScriptInput> inputs{vnmaki::ChooseInput{.index = 2}};
    const HeadlessResult result = vnmaki::run_headless(vfs, inputs);

    CHECK(result.outcome == HeadlessOutcome::kFailed);
    CHECK(result.error ==
          "the story waits at a menu, and the next input is not one of its options");
  }
  SUBCASE("a choice does not answer a line") {
    const std::vector<ScriptInput> inputs{vnmaki::ChooseInput{.index = 0}, vnmaki::ChooseInput{}};
    const HeadlessResult result = vnmaki::run_headless(vfs, inputs);

    CHECK(result.outcome == HeadlessOutcome::kFailed);
    CHECK(result.error == "the next input is a menu choice, but the story does not wait at a menu");
  }
}
