// Fuzzes the script loader and runtime (engine/runtime): game scripts are
// untrusted input. The input becomes scripts/fuzz.lua, which the headless
// runner loads and runs with memory and instruction limits, so runaway
// scripts fail instead of exhausting the fuzzer.
//
// The first byte picks how the rest becomes a script:
// - below 128: the bytes are the script text as-is (exercises the Lua loader);
// - otherwise (statement mode): each byte picks a statement from
//   kStatements for the body of label "start", and byte 255 moves on to the
//   bodies of labels "a" and "b". Nearly every such input is valid Lua that
//   says lines and moves between labels (exercises the label registry,
//   jump/call/ret, yields, the limits and the sandbox).
//
// Besides "does not crash", every result must be consistent:
// - it failed exactly when it has an error message;
// - every transcript line has a known form;
// - each yield point (stop) belongs to the label the transcript says is
//   running, its step is 1 right after the label is entered, and it grows
//   by 1 between consecutive stops of the same running label.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/runtime/headless_runner.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

constexpr std::size_t kMemoryLimit = std::size_t{32} << 20U;
constexpr std::uint32_t kInstructionLimit = 20'000;
constexpr std::size_t kAdvances = 16;
constexpr std::uint8_t kStatementModeThreshold = 128;
// In statement mode this byte ends the current label's body.
constexpr std::uint8_t kNextLabel = 255;

// The labels statement mode defines, in order.
constexpr std::array<std::string_view, 3> kLabels{"start", "a", "b"};

// Complete Lua statements, so statement-mode scripts are nearly always valid
// Lua and the fuzzer spends its time in the runtime, not the parser.
constexpr std::array<std::string_view, 40> kStatements{
    "narrate(\"n\")",
    "narrate(\"m\")",
    "say(\"A\", \"s\")",
    "say(nil, \"x\")",
    "say({ name = \"C\" }, \"c\")",
    "jump(\"a\")",
    "jump(\"b\")",
    "jump(\"start\")",
    "jump(\"missing\")",
    "call(\"a\")",
    "call(\"b\")",
    "call(\"start\")",
    "ret()",
    "if math.random(2) == 1 then narrate(\"r\") end",
    "if store.x and store.x > 2 then ret() end",
    "for _ = 1, 3 do narrate(\"loop\") end",
    "while true do end",
    "store.x = (store.x or 0) + 1",
    "global_value = (global_value or 0) + 1",
    "print(store.x, math.random(), math.random(0))",
    "coroutine.yield()",
    "error(\"boom\")",
    "error({}, 0)",
    "say({}, 1)",
    "jump(1)",
    "label(\"late\", function() end)",
    "print(vfs.read(\"scripts/fuzz.lua\") ~= nil, vfs.exists(\"x\"), #vfs.list(\"\"))",
    "coroutine.wrap(function() narrate(\"c\") end)()",
    "pcall(narrate, \"p\")",
    "pcall(jump, \"a\")",
    "print(pcall(error, \"caught\"))",
    "do return end",
    "local s = string.rep(\"x\", 1 << 30)",
    "store.big = string.rep(\"x\", 1 << 20)",
    "print(load(\"return 1\")(), load(\"\\27Lua\"))",
    "setmetatable(_G, nil)",
    "local t = setmetatable({}, { __index = function() narrate(\"meta\") end }) local _ = t.x",
    "local co = coroutine.create(function() while true do end end) print(coroutine.resume(co))",
    "print(os.time() ~= nil, io, debug, require)",
    "repeat store.y = (store.y or 0) + 1 narrate(\"rep\") until store.y % 2 == 0",
};

void require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

// Statement mode: the bytes are statement indices, split into the bodies of
// the labels in kLabels by kNextLabel bytes.
std::string statement_script(std::span<const std::uint8_t> bytes) {
  std::string script;
  std::size_t label = 0;
  script += "label(\"";
  script += kLabels.at(label);
  script += "\", function()\n";
  for (const std::uint8_t byte : bytes) {
    if (byte == kNextLabel) {
      if (label + 1 == kLabels.size()) {
        break;
      }
      ++label;
      script += "end)\nlabel(\"";
      script += kLabels.at(label);
      script += "\", function()\n";
      continue;
    }
    script += kStatements.at(byte % kStatements.size());
    script += '\n';
  }
  script += "end)\n";
  return script;
}

std::string script_from(std::span<const std::uint8_t> bytes) {
  if (bytes.empty()) {
    return {};
  }
  if (bytes.front() >= kStatementModeThreshold) {
    return statement_script(bytes.subspan(1));
  }
  std::string script;
  for (const std::uint8_t byte : bytes.subspan(1)) {
    script.push_back(static_cast<char>(byte));
  }
  return script;
}

bool is_stop_line(std::string_view line) {
  return line.starts_with("say ") || line.starts_with("narrate ");
}

void check(const vnmaki::HeadlessResult& result) {
  require((result.outcome == vnmaki::HeadlessOutcome::kFailed) == !result.error.empty());

  std::string running;
  std::uint32_t expected_step = 0;  // 0: any step >= 1 (after a resume)
  std::size_t next_stop = 0;
  for (const std::string& line : result.transcript) {
    if (line.starts_with("label ")) {
      running = line.substr(std::string_view{"label "}.size());
      expected_step = 1;
    } else if (line.starts_with("resume ")) {
      running = line.substr(std::string_view{"resume "}.size());
      expected_step = 0;
    } else if (is_stop_line(line)) {
      require(next_stop < result.stops.size());
      const vnmaki::ScriptPosition& stop = result.stops[next_stop++];
      require(stop.label == running);
      require(stop.step >= 1);
      require(expected_step == 0 || stop.step == expected_step);
      expected_step = stop.step + 1;
    } else {
      require(line.starts_with("print ") || line.starts_with("warning "));
    }
  }
  require(next_stop == result.stops.size());
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const vnmaki::Vfs vfs =
      vnmaki::test::memory_vfs({{"scripts/fuzz.lua", script_from(std::span{data, size})}});

  vnmaki::HeadlessOptions options;
  options.runtime.memory_limit_bytes = kMemoryLimit;
  options.runtime.instruction_limit = kInstructionLimit;
  const std::vector<vnmaki::ScriptInput> inputs(kAdvances, vnmaki::AdvanceInput{});

  check(vnmaki::run_headless(vfs, inputs, options));
  return 0;
}
