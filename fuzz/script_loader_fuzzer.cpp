// Fuzzes the script loader and runtime (engine/runtime): game scripts are
// untrusted input. The input becomes scripts/fuzz.lua, which the headless
// runner loads and runs with memory and instruction limits, so runaway
// scripts fail instead of exhausting the fuzzer.
//
// The first byte picks how the rest becomes a script:
// - below 128: the bytes are the script text as-is (exercises the Lua loader);
// - otherwise: each byte picks a line from kSnippets, so most inputs are
//   valid Lua that defines labels and moves between them (exercises the
//   label registry, jump/call/ret, yields and the sandbox).
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
constexpr std::uint8_t kSnippetModeThreshold = 128;

constexpr std::array<std::string_view, 40> kSnippets{
    "label(\"start\", function()\n",
    "label(\"a\", function()\n",
    "label(\"b\", function()\n",
    "end)\n",
    "end\n",
    "narrate(\"n\")\n",
    "say(\"A\", \"s\")\n",
    "say(nil, \"x\")\n",
    "say({ name = \"C\" }, \"c\")\n",
    "jump(\"a\")\n",
    "jump(\"b\")\n",
    "jump(\"start\")\n",
    "jump(\"missing\")\n",
    "call(\"a\")\n",
    "call(\"b\")\n",
    "ret()\n",
    "if math.random(2) == 1 then\n",
    "else\n",
    "for i = 1, 3 do\n",
    "while true do\n",
    "do\n",
    "store.x = (store.x or 0) + 1\n",
    "global_value = 1\n",
    "print(store.x, math.random())\n",
    "coroutine.yield()\n",
    "error(\"boom\")\n",
    "error({}, 0)\n",
    "say({}, 1)\n",
    "jump(1)\n",
    "label(\"start\", 5)\n",
    "print(vfs.read(\"scripts/fuzz.lua\") ~= nil, vfs.exists(\"x\"), #vfs.list(\"\"))\n",
    "coroutine.wrap(function() narrate(\"c\") end)()\n",
    "pcall(narrate, \"p\")\n",
    "pcall(jump, \"a\")\n",
    "return\n",
    "break\n",
    "local s = string.rep(\"x\", 1 << 30)\n",
    "print(load(\"return 1\")())\n",
    "setmetatable(_G, nil)\n",
    "local t = setmetatable({}, { __index = function() narrate(\"meta\") end }) local _ = t.x\n",
};

void require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

std::string script_from(std::span<const std::uint8_t> bytes) {
  std::string script;
  if (bytes.empty()) {
    return script;
  }
  const bool snippets = bytes.front() >= kSnippetModeThreshold;
  for (const std::uint8_t byte : bytes.subspan(1)) {
    if (snippets) {
      script += kSnippets.at(byte % kSnippets.size());
    } else {
      script.push_back(static_cast<char>(byte));
    }
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
