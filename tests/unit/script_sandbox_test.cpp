// The standard library and engine API game scripts see (docs/SPEC.md
// sections 7 and 8), observed through the headless runner: scripts print
// what they find, and the tests read it back from the transcript.

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "engine/runtime/headless_runner.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::HeadlessOptions;
using vnmaki::HeadlessOutcome;
using vnmaki::HeadlessResult;
using Lines = std::vector<std::string>;

using Files = std::map<std::string, std::string, std::less<>>;

HeadlessResult run_files(Files files, const HeadlessOptions& options = {}) {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs(std::move(files));
  constexpr std::size_t kAdvances = 16;
  const std::vector<vnmaki::ScriptInput> inputs(kAdvances, vnmaki::AdvanceInput{});
  return vnmaki::run_headless(vfs, inputs, options);
}

HeadlessResult run_script(const std::string& source, const HeadlessOptions& options = {}) {
  return run_files({{"scripts/main.lua", source}}, options);
}

// Runs `body` inside the start label and returns what it printed.
Lines printed_by(const std::string& body) {
  const HeadlessResult result = run_script("label('start', function()\n" + body + "\nend)\n");
  REQUIRE_MESSAGE(result.outcome == HeadlessOutcome::kFinished, result.error);
  Lines printed;
  for (const std::string& line : result.transcript) {
    if (line.starts_with("print ")) {
      printed.push_back(line.substr(std::string{"print "}.size()));
    }
  }
  return printed;
}

}  // namespace

TEST_CASE("sandbox: no file, process or debug access from the standard library") {
  CHECK(printed_by(R"(
print(io, debug, package, require, dofile, loadfile)
print(string.dump, math.randomseed)
print(os.execute, os.exit, os.getenv, os.remove, os.rename, os.tmpname, os.setlocale)
)") == Lines{"nil\tnil\tnil\tnil\tnil\tnil", "nil\tnil", "nil\tnil\tnil\tnil\tnil\tnil\tnil"});
}

TEST_CASE("sandbox: the safe parts of the standard library are there") {
  CHECK(printed_by(R"(
print(type(string.format), type(table.concat), type(math.floor), type(utf8.char))
print(type(coroutine.wrap), type(pcall), type(setmetatable))
print(type(os.time), type(os.clock), type(os.date), type(os.difftime))
)") == Lines{"function\tfunction\tfunction\tfunction", "function\tfunction\tfunction",
             "function\tfunction\tfunction\tfunction"});
}

TEST_CASE("sandbox: load compiles source text but never binary chunks") {
  CHECK(printed_by(R"(
print(load("return 1 + 2")())
print(load("return x", "chunk", "t", { x = "from env" })())
print(load("\27Lua binary", "chunk", "b"))
)") == Lines{"3", "from env", "nil\tattempt to load a binary chunk (mode is 't')"});
}

TEST_CASE("sandbox: scripts read files through the vfs table") {
  const HeadlessResult result = run_files({
      {"scripts/main.lua", R"(label("start", function()
  print(vfs.read("data/greeting.txt"))
  print(vfs.read("data/missing.txt"), vfs.exists("data/greeting.txt"), vfs.exists("nope"))
  print(vfs.read("../outside.txt"))
  for _, entry in ipairs(vfs.list("data")) do
    print(entry.name, entry.is_directory)
  end
end)
)"},
      {"data/greeting.txt", "hello"},
      {"data/sub/inner.txt", "inner"},
  });

  CHECK(result.outcome == HeadlessOutcome::kFinished);
  CHECK(result.transcript == Lines{"label start", "print hello", "print nil\ttrue\tfalse",
                                   "print nil", "print greeting.txt\tfalse", "print sub\ttrue"});
}

TEST_CASE("sandbox: math.random is deterministic and follows Lua's argument rules") {
  const std::string body = R"(
local ok = true
for _ = 1, 200 do
  local d = math.random(6)
  local r = math.random(-3, 3)
  local f = math.random()
  ok = ok and math.type(d) == "integer" and d >= 1 and d <= 6
  ok = ok and r >= -3 and r <= 3 and f >= 0 and f < 1
end
print(ok, math.type(math.random(0)), math.random(7, 7))
print(math.random(1, 1000000), math.random(1, 1000000))
print(pcall(math.random, 2, 1))
print(pcall(math.random, 1.5))
)";
  const Lines first = printed_by(body);
  REQUIRE(first.size() == 4);
  CHECK(first[0] == "true\tinteger\t7");
  CHECK(first[2] == "false\tbad argument #2 to 'random' (interval is empty)");
  CHECK(first[3] == "false\tbad argument #1 to 'random' (number has no integer representation)");
  // Every run draws the same numbers.
  CHECK(printed_by(body) == first);
}

TEST_CASE("sandbox: dev mode warns once when a script writes a new global") {
  const HeadlessResult result = run_script(R"(
counter = 1
label("start", function()
  local fine = 1
  store.affection = 2
  counter = counter + 1
  leaked = true
  leaked = false
end)
)");

  CHECK(result.outcome == HeadlessOutcome::kFinished);
  CHECK(result.transcript ==
        Lines{
            "warning scripts/main.lua:2: new global 'counter' (keep game state in store, or use "
            "a local)",
            "label start",
            "warning scripts/main.lua:7: new global 'leaked' (keep game state in store, or use "
            "a local)",
        });
}

TEST_CASE("sandbox: outside dev mode new globals are not reported") {
  HeadlessOptions options;
  options.runtime.dev_mode = false;

  const HeadlessResult result = run_script("label('start', function() leaked = true end)", options);

  CHECK(result.outcome == HeadlessOutcome::kFinished);
  CHECK(result.transcript == Lines{"label start"});
}

TEST_CASE("sandbox: a memory limit turns runaway allocation into a script error") {
  HeadlessOptions options;
  constexpr std::size_t kLimit = std::size_t{4} << 20U;
  options.runtime.memory_limit_bytes = kLimit;

  const HeadlessResult result =
      run_script("label('start', function() local s = string.rep('x', 64 << 20) end)", options);

  CHECK(result.outcome == HeadlessOutcome::kFailed);
  CHECK(result.error == "not enough memory");
}

TEST_CASE("sandbox: an instruction limit turns an endless loop into a script error") {
  HeadlessOptions options;
  constexpr std::uint32_t kLimit = 100'000;
  options.runtime.instruction_limit = kLimit;
  const std::string expected = "script ran too long without yielding (instruction limit reached)";

  SUBCASE("in a label") {
    const HeadlessResult result =
        run_script("label('start', function()\n  while true do end\nend)", options);
    CHECK(result.outcome == HeadlessOutcome::kFailed);
    CHECK(result.error == "scripts/main.lua:2: " + expected);
  }
  SUBCASE("in top-level script code") {
    const HeadlessResult result = run_script("while true do end", options);
    CHECK(result.outcome == HeadlessOutcome::kFailed);
    CHECK(result.error == "scripts/main.lua:1: " + expected);
  }
  SUBCASE("in a coroutine the script made") {
    const HeadlessResult result = run_script(
        "label('start', function()\n  coroutine.wrap(function() while true do end end)()\nend)",
        options);
    CHECK(result.outcome == HeadlessOutcome::kFailed);
    CHECK(result.error == "scripts/main.lua:2: " + expected);
  }
  SUBCASE("in labels that jump to each other without yielding") {
    const HeadlessResult result =
        run_script("label('start', function() jump('start') end)", options);
    CHECK(result.outcome == HeadlessOutcome::kFailed);
    CHECK(result.error == "scripts/main.lua:1: " + expected);
  }
  SUBCASE("but every advance gets a fresh budget") {
    const HeadlessResult result = run_script(R"(label('start', function()
  for _ = 1, 20 do
    for _ = 1, 10000 do end
    narrate("tick")
  end
end))",
                                             options);
    CHECK(result.outcome == HeadlessOutcome::kOutOfInput);
  }
}
