// character{...} through ScriptRuntime's public interface: what a say request carries.

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include <doctest/doctest.h>

#include "engine/runtime/script_runtime.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::RunState;
using vnmaki::SayRequest;

constexpr std::uint32_t kLilac = 0xC8A2C8;

vnmaki::Vfs script(const std::string& source) {
  return vnmaki::test::memory_vfs({{"scripts/main.lua", source}});
}

}  // namespace

TEST_CASE("character: a say request carries the speaker name, colour and voice tag") {
  const vnmaki::Vfs vfs = script(R"(
local e = character { name = "Eileen", color = "#c8a2c8", voice_tag = "eileen" }
label("start", function() say(e, "You came back.") end)
)");
  vnmaki::ScriptRuntime runtime{vfs};
  runtime.load_scripts();
  runtime.start("start");

  REQUIRE(runtime.state() == RunState::kWaiting);
  const auto* say = std::get_if<SayRequest>(runtime.request());
  REQUIRE(say != nullptr);
  CHECK(say->speaker == "Eileen");
  CHECK(say->color == std::optional<std::uint32_t>{kLilac});
  CHECK(say->voice_tag == "eileen");
  CHECK(say->text == "You came back.");
}

TEST_CASE("character: colour and voice tag are optional, and a plain string has neither") {
  const vnmaki::Vfs vfs = script(R"(
local lucy = character { name = "Lucy" }
label("start", function()
  say(lucy, "one")
  say("Anna", "two")
end)
)");
  vnmaki::ScriptRuntime runtime{vfs};
  runtime.load_scripts();
  runtime.start("start");
  const auto* first = std::get_if<SayRequest>(runtime.request());
  REQUIRE(first != nullptr);
  CHECK(first->speaker == "Lucy");
  CHECK_FALSE(first->color.has_value());
  CHECK(first->voice_tag.empty());

  runtime.advance();
  const auto* second = std::get_if<SayRequest>(runtime.request());
  REQUIRE(second != nullptr);
  CHECK(second->speaker == "Anna");
  CHECK_FALSE(second->color.has_value());
}

TEST_CASE("character: a bad definition is a script error at the calling line") {
  const vnmaki::Vfs vfs = script(R"(
label("start", function()
  local bad = character { name = "Eileen", color = "lilac" }
  say(bad, "never")
end)
)");
  vnmaki::ScriptRuntime runtime{vfs};
  runtime.load_scripts();
  runtime.start("start");

  CHECK(runtime.state() == RunState::kFailed);
  CHECK(runtime.error().find("scripts/main.lua:3:") != std::string::npos);
  CHECK(runtime.error().find("color") != std::string::npos);
}
