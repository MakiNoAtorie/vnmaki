// The dialogue presenter seam: a ScriptRuntime over memory scripts, explicit elapsed seconds
// instead of a clock, and a glyph counter that counts bytes (the test text is ASCII).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <doctest/doctest.h>

#include "engine/dialogue/dialogue_presenter.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::DialogueLine;
using vnmaki::DialoguePresenter;
using vnmaki::RunState;

constexpr double kSpeed = 10;  // characters per second
constexpr double kHalfSecond = 0.5;
constexpr double kOneSecond = 1;
constexpr double kTenSeconds = 10;
constexpr std::size_t kHelloLength = 5;
constexpr std::size_t kFullLength = 12;
constexpr std::size_t kLineCount = 2;
constexpr std::uint32_t kLilac = 0xC8A2C8;

constexpr auto kScript = R"(
local eileen = character { name = "Eileen", color = "#c8a2c8" }
label("start", function()
  say(eileen, "Hello there!")
  narrate("Rain.")
end)
)";

struct Game {
  vnmaki::Vfs vfs = vnmaki::test::memory_vfs({{"scripts/main.lua", kScript}});
  vnmaki::ScriptRuntime runtime{vfs};
  std::size_t counted = 0;
  std::unique_ptr<DialoguePresenter> presenter;

  Game() {
    runtime.load_scripts();
    runtime.start("start");
    presenter = std::make_unique<DialoguePresenter>(
        runtime,
        [this](const DialogueLine& line) {
          ++counted;
          return line.text.size();
        },
        kSpeed);
  }
};

// The current line, or an empty one. The tests REQUIRE that a line shows first.
vnmaki::DialogueLine shown(const Game& game) {
  return game.presenter->line().value_or(vnmaki::DialogueLine{});
}

}  // namespace

TEST_CASE("dialogue: the current line has its speaker, colour and text, with nothing revealed") {
  Game game;
  const auto& line = game.presenter->line();

  REQUIRE(line.has_value());
  CHECK(shown(game).speaker == "Eileen");
  CHECK(shown(game).color == std::optional<std::uint32_t>{kLilac});
  CHECK(shown(game).text == "Hello there!");
  CHECK(game.presenter->visible_glyphs() == 0);
  CHECK(game.presenter->revealing());
}

TEST_CASE("dialogue: the reveal follows elapsed time") {
  Game game;
  game.presenter->update(kHalfSecond);
  CHECK(game.presenter->visible_glyphs() == kHelloLength);
  game.presenter->update(kOneSecond);
  CHECK(game.presenter->visible_glyphs() == kFullLength);
  CHECK_FALSE(game.presenter->revealing());
}

TEST_CASE("dialogue: the first advance completes the line and the next one moves on") {
  Game game;
  game.presenter->update(kHalfSecond);

  game.presenter->advance();
  REQUIRE(game.presenter->line().has_value());
  CHECK(shown(game).text == "Hello there!");
  CHECK(game.presenter->visible_glyphs() == kFullLength);
  CHECK_FALSE(game.presenter->revealing());

  game.presenter->advance();
  REQUIRE(game.presenter->line().has_value());
  CHECK(shown(game).text == "Rain.");
  CHECK(shown(game).speaker.empty());
  CHECK_FALSE(shown(game).color.has_value());
  CHECK(game.presenter->visible_glyphs() == 0);
  CHECK(game.runtime.position().step == kLineCount);
}

TEST_CASE("dialogue: a line that finished on its own needs only one advance") {
  Game game;
  game.presenter->update(kTenSeconds);
  game.presenter->advance();

  REQUIRE(game.presenter->line().has_value());
  CHECK(shown(game).text == "Rain.");
}

TEST_CASE("dialogue: after the last line the story ends cleanly") {
  Game game;
  game.presenter->advance();  // completes "Hello there!"
  game.presenter->advance();  // moves to "Rain."
  game.presenter->update(kOneSecond);
  CHECK(shown(game).text == "Rain.");
  CHECK_FALSE(game.presenter->ended());

  game.presenter->advance();  // past the last line

  CHECK(game.runtime.state() == RunState::kFinished);
  CHECK(game.presenter->ended());
  CHECK_FALSE(game.presenter->line().has_value());
  CHECK(game.presenter->visible_glyphs() == 0);
  game.presenter->advance();
  game.presenter->update(kOneSecond);
  CHECK(game.presenter->ended());
}

TEST_CASE("dialogue: the glyph counter runs once for each line") {
  Game game;
  game.presenter->update(kOneSecond);
  game.presenter->advance();
  game.presenter->advance();
  game.presenter->update(kOneSecond);
  CHECK(game.counted == kLineCount);
}
