// The dev console's Lua REPL and log (engine/tools/repl.hpp, ScriptRuntime::eval).

#include "engine/tools/repl.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "engine/runtime/script_runtime.hpp"
#include "engine/tools/log_buffer.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::LogBuffer;
using vnmaki::LogLevel;

constexpr std::size_t kSmallCapacity = 3;
constexpr std::size_t kManyLines = 5;
constexpr std::uint32_t kInstructionLimit = 100000;

// A runtime with one script, started, that logs to `log`.
struct Session {
  vnmaki::Vfs vfs;
  LogBuffer log;
  vnmaki::ScriptRuntime runtime;

  explicit Session(const std::string& script)
      : vfs(vnmaki::test::memory_vfs({{"scripts/main.lua", script}})),
        runtime(vfs, {.observer = &log}) {
    runtime.load_scripts();
  }
};

const char* const kStory =
    "store.gold = 5\n"
    "label('start', function()\n"
    "  narrate('a')\n"
    "  narrate('b')\n"
    "end)\n";

}  // namespace

TEST_CASE("repl: an expression prints its value") {
  Session session{kStory};
  vnmaki::run_repl_line(session.runtime, session.log, "1 + 2");
  REQUIRE(session.log.entries().size() == 2);
  CHECK(session.log.entries().at(0).text == "> 1 + 2");
  CHECK(session.log.entries().at(1).level == LogLevel::kInfo);
  CHECK(session.log.entries().at(1).text == "3");
}

TEST_CASE("repl: statements print nothing and keep their effect") {
  Session session{kStory};
  vnmaki::run_repl_line(session.runtime, session.log, "store.x = 4");
  CHECK(session.log.entries().size() == 1);  // only the echo
  session.log.clear();
  vnmaki::run_repl_line(session.runtime, session.log, "store.x * 2, 'a'");
  CHECK(session.log.entries().back().text == "8\ta");
}

TEST_CASE("repl: a table is listed one level deep") {
  Session session{kStory};
  const vnmaki::EvalResult result = session.runtime.eval("{n = 1, inner = {1, 2}}");
  CHECK(result.ok);
  CHECK(result.text.starts_with("{"));
  CHECK(result.text.find("n = 1") != std::string::npos);
  CHECK(result.text.find("inner = {...}") != std::string::npos);
}

TEST_CASE("repl: an error is logged as an error and the runtime goes on") {
  Session session{kStory};
  vnmaki::run_repl_line(session.runtime, session.log, "error('boom')");
  CHECK(session.log.entries().back().level == LogLevel::kError);
  CHECK(session.log.entries().back().text.find("boom") != std::string::npos);
  vnmaki::run_repl_line(session.runtime, session.log, "1 +");  // a syntax error
  CHECK(session.log.entries().back().level == LogLevel::kError);
  vnmaki::run_repl_line(session.runtime, session.log, "2 + 2");
  CHECK(session.log.entries().back().text == "4");
}

TEST_CASE("repl: print goes to the log through the observer") {
  Session session{kStory};
  vnmaki::run_repl_line(session.runtime, session.log, "print('hi', 7)");
  REQUIRE(session.log.entries().size() == 2);
  CHECK(session.log.entries().at(1).text == "hi\t7");
}

TEST_CASE("repl: it runs in the script sandbox") {
  Session session{kStory};
  for (const char* name : {"io", "debug", "package", "require", "dofile", "loadfile"}) {
    const vnmaki::EvalResult result = session.runtime.eval(std::string{"type("} + name + ")");
    CHECK_MESSAGE(result.ok, name);
    CHECK_MESSAGE(result.text == "nil", name);
  }
  CHECK(session.runtime.eval("os.execute").text == "nil");
  CHECK(session.runtime.eval("store.gold").text == "5");  // it sees the game's globals
}

TEST_CASE("repl: a blocking call is an error, not a hang") {
  Session session{kStory};
  const vnmaki::EvalResult result = session.runtime.eval("narrate('x')");
  CHECK_FALSE(result.ok);
}

TEST_CASE("repl: a change to store lasts until a rollback or load") {
  Session session{kStory};
  session.runtime.start("start");
  session.runtime.advance();  // waits at say 'b'
  REQUIRE(session.runtime.can_roll_back());
  CHECK(session.runtime.eval("store.gold = 9").ok);
  CHECK(session.runtime.eval("store.gold").text == "9");
  CHECK(session.runtime.roll_back());
  CHECK(session.runtime.eval("store.gold").text == "5");
}

TEST_CASE("repl: a runaway loop stops at the instruction limit") {
  const vnmaki::Vfs vfs =
      vnmaki::test::memory_vfs({{"scripts/main.lua", "label('start', function() end)"}});
  vnmaki::ScriptRuntime runtime{vfs, {.instruction_limit = kInstructionLimit}};
  runtime.load_scripts();
  CHECK_FALSE(runtime.eval("while true do end").ok);
}

TEST_CASE("log: the buffer keeps the last entries and counts all") {
  LogBuffer log{kSmallCapacity};
  for (std::size_t i = 0; i < kManyLines; ++i) {
    log.add(LogLevel::kWarning, std::to_string(i));
  }
  CHECK(log.entries().size() == kSmallCapacity);
  CHECK(log.entries().front().text == "2");
  CHECK(log.total() == kManyLines);
  log.clear();
  CHECK(log.entries().empty());
  CHECK(log.total() == kManyLines);
}
