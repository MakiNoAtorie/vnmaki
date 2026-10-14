// Text tags in scripts (docs/TEXT_TAGS.md): errors at the script line, and the link callback.

#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/runtime/script_runtime.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::RunState;
using Lines = std::vector<std::string>;

class Printed final : public vnmaki::ScriptObserver {
 public:
  void on_print(std::string_view text) override { lines.emplace_back(text); }

  Lines lines;
};

struct Game {
  explicit Game(const std::string& source)
      : vfs(vnmaki::test::memory_vfs({{"scripts/main.lua", source}})),
        runtime(vfs, {.observer = &printed}) {
    runtime.load_scripts();
  }

  vnmaki::Vfs vfs;
  Printed printed;
  vnmaki::ScriptRuntime runtime;
};

}  // namespace

TEST_CASE("script text: text with good tags is said as written") {
  Game game{R"(
label("start", function()
  say("Kai", "I {color=#f00}really{/color} mean it.{pause=0.5} Truly.")
end)
)"};
  game.runtime.start("start");

  REQUIRE_MESSAGE(game.runtime.state() == RunState::kWaiting, game.runtime.error());
  const auto* said = std::get_if<vnmaki::SayRequest>(game.runtime.request());
  REQUIRE(said != nullptr);
  CHECK(said->text == "I {color=#f00}really{/color} mean it.{pause=0.5} Truly.");
}

TEST_CASE("script text: a bad tag fails the story at the script line of the call") {
  Game game{R"(
label("start", function()
  narrate("A {b}bold{/b} start.")
  say("Kai", "I {colr=#f00}mean{/colr} it.")
end)
)"};
  game.runtime.start("start");
  REQUIRE_MESSAGE(game.runtime.state() == RunState::kWaiting, game.runtime.error());
  game.runtime.advance();

  CHECK(game.runtime.state() == RunState::kFailed);
  CHECK(game.runtime.error() ==
        "scripts/main.lua:4: bad text tag at byte 2: unknown tag 'colr'");

  Game other{R"(
label("open", function()
  narrate("Never {i}closed.")
end)
)"};
  other.runtime.start("open");
  CHECK(other.runtime.error() == "scripts/main.lua:3: bad text tag at byte 6: {i} is not closed");
}

TEST_CASE("script text: a link calls the handler the script gave to on_link") {
  Game game{R"(
on_link(function(id)
  print("link " .. id)
end)
label("start", function()
  narrate("Go to the {link=shop}shop{/link}.")
end)
)"};
  // No text is shown before the story waits.
  CHECK_FALSE(game.runtime.activate_link("shop"));
  game.runtime.start("start");

  CHECK(game.runtime.activate_link("shop"));
  CHECK(game.printed.lines == Lines{"link shop"});
  CHECK(game.runtime.state() == RunState::kWaiting);
}

TEST_CASE("script text: a link without a handler does nothing") {
  Game game{R"(
label("start", function()
  narrate("{link=a}A{/link}")
end)
)"};
  game.runtime.start("start");

  CHECK_FALSE(game.runtime.activate_link("a"));
  CHECK(game.runtime.state() == RunState::kWaiting);
}

TEST_CASE("script text: an error in the link handler fails the story") {
  Game game{R"(
on_link(function(id)
  say("Kai", "Links cannot talk.")
end)
label("start", function()
  narrate("{link=a}A{/link}")
end)
)"};
  game.runtime.start("start");

  CHECK_FALSE(game.runtime.activate_link("a"));
  CHECK(game.runtime.state() == RunState::kFailed);
  CHECK(game.runtime.error() ==
        "scripts/main.lua:3: say() can only be called from a running label");
}

TEST_CASE("script text: on_link takes only a function") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({{"scripts/main.lua", "on_link(3)\n"}});
  vnmaki::ScriptRuntime runtime{vfs};
  CHECK_THROWS_WITH_AS(runtime.load_scripts(),
                       "scripts/main.lua:1: bad argument #1 to 'on_link' (function expected, got "
                       "number)",
                       vnmaki::ScriptError);
}
