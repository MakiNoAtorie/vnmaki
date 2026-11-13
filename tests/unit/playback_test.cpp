// Persistent store, seen text, skip and auto-advance (docs/SPEC.md section 8). Time is explicit:
// the tests pass elapsed seconds to the presenter and to Playback, so there is no clock. Expected
// values are written out by hand from the scripts.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/dialogue/dialogue_presenter.hpp"
#include "engine/dialogue/playback.hpp"
#include "engine/runtime/headless_runner.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/saves/persistent_store.hpp"
#include "engine/saves/save_file.hpp"
#include "engine/saves/saves.hpp"
#include "engine/ui/playback_indicator.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"
#include "tests/support/temp_dir.hpp"

namespace {

using vnmaki::DialogueLine;
using vnmaki::DialoguePresenter;
using vnmaki::Playback;
using vnmaki::PlaybackOptions;
using vnmaki::RunState;
using vnmaki::SkipMode;

constexpr double kSpeed = 10;  // characters per second
constexpr double kTick = 0.125;
constexpr double kBaseDelay = 1.0;
constexpr double kGlyphDelay = 0.125;
constexpr double kSkipInterval = 0.125;
constexpr double kLongTime = 100;
constexpr double kHelloDelay = 1.625;
constexpr double kSlowHelloDelay = 2.25;
constexpr double kMultiplier = 2;
constexpr std::size_t kHelloLength = 5;
constexpr std::size_t kMaxTicks = 200;
// "one" takes 0.3 s to type: 3 ticks. The wait of 1.0 + 3 * 0.125 s is 11 ticks, the first of
// them in the tick that finishes the reveal.
constexpr std::size_t kTicksForOne = 13;
// "Hello" takes 0.5 s to type: 4 ticks. The wait of 1.0 + 5 * 0.125 s then needs 13 ticks, the
// first of them in the tick that finishes the reveal.
constexpr std::size_t kTicksUntilAdvance = 16;
// A multiplier of 2 doubles the glyph part: 1.0 + 5 * 0.125 * 2 s is 18 ticks after the reveal.
constexpr std::size_t kTicksUntilSlowAdvance = 21;
// After a voice, only the base delay counts: 8 ticks.
constexpr std::size_t kTicksAfterVoice = 8;
constexpr std::size_t kTwoLines = 2;
constexpr std::size_t kThreeLines = 3;

// A directory under the system temp directory, empty at the start and removed at the end.
class TempDir {
 public:
  explicit TempDir(const std::string& name)
      : path_(vnmaki::test::unique_temp_path("vnmaki_playback_test_", name)) {
    std::filesystem::remove_all(path_);
  }
  TempDir(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir& operator=(TempDir&&) = delete;
  ~TempDir() {
    try {
      std::filesystem::remove_all(path_);
    } catch (...) {
      std::fputs("TempDir: cannot remove the directory\n", stderr);
    }
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

constexpr auto kStory = R"(
label("start", function()
  say("A", "one")
  say("A", "two")
  say("A", "three")
  local pick = menu { "x", "y" }
  say("A", "four")
end)
)";

constexpr auto kEditedStory = R"(
label("start", function()
  say("A", "one")
  say("A", "two, edited")
  say("A", "three")
end)
)";

constexpr auto kHelloStory = R"(
local slow = character { name = "S", auto_multiplier = 2 }
label("start", function()
  say("F", "Hello")
  say(slow, "Hello")
  narrate("end")
end)
)";

// A story, its runtime, the persistent store in `directory`, a presenter and Playback.
struct Game {
  Game(const std::string& source, const std::filesystem::path& directory,
       PlaybackOptions options = {})
      : vfs(vnmaki::test::memory_vfs({{"scripts/main.lua", source}})),
        runtime(vfs),
        persistent(runtime, directory,
                   [this](std::string_view message) { warnings.emplace_back(message); }) {
    persistent.load();
    runtime.load_scripts();
    runtime.start("start");
    presenter = std::make_unique<DialoguePresenter>(
        runtime, [](const DialogueLine& line) { return line.text.size(); }, kSpeed);
    playback = std::make_unique<Playback>(runtime, *presenter, persistent, std::move(options));
  }

  // One frame: the presenter first, then Playback, as a host does.
  void tick(double seconds = kTick) {
    presenter->update(seconds);
    playback->update(seconds);
  }

  // The player reads the line that shows: it types out, and the player advances.
  void read_line() {
    tick(kLongTime);
    presenter->advance();
  }

  [[nodiscard]] std::string shown() const {
    return presenter->line() ? presenter->line()->text : std::string{};
  }

  // Ticks until the presenter shows another line; the number of ticks, or kMaxTicks + 1.
  std::size_t ticks_until_next_line() {
    const std::uint64_t serial = presenter->line_serial();
    for (std::size_t n = 1; n <= kMaxTicks; ++n) {
      tick();
      if (presenter->line_serial() != serial) {
        return n;
      }
    }
    return kMaxTicks + 1;
  }

  // Ticks while skip is `mode`, up to kMaxTicks.
  void tick_while_skipping(SkipMode mode) {
    for (std::size_t n = 0; n < kMaxTicks && playback->skip() == mode; ++n) {
      tick();
    }
  }

  vnmaki::Vfs vfs;
  vnmaki::ScriptRuntime runtime;
  std::vector<std::string> warnings;
  vnmaki::PersistentStore persistent;
  std::unique_ptr<DialoguePresenter> presenter;
  std::unique_ptr<Playback> playback;
};

// The value of `key` in the persistent table, as a string; empty if it is missing.
std::string persistent_text(const vnmaki::ScriptRuntime& runtime, std::string_view key) {
  const vnmaki::StoreSnapshot snapshot = runtime.persistent_snapshot();
  for (const auto& [name, value] : snapshot.tables.at(0).entries) {
    const auto* text = std::get_if<std::string>(&name);
    if (text != nullptr && *text == key) {
      const auto* result = std::get_if<std::string>(&value);
      return result != nullptr ? *result : std::string{};
    }
  }
  return {};
}

}  // namespace

TEST_CASE("persistent: a value written by a script is saved and read in the next run") {
  const TempDir dir{"roundtrip"};
  constexpr auto kScript = R"(
persistent.mood = persistent.mood or "new"
label("start", function()
  persistent.mood = "seen"
  narrate("hi")
end)
)";
  {
    Game first{kScript, dir.path()};
    CHECK(persistent_text(first.runtime, "mood") == "seen");
  }  // the store writes its file when it goes away
  const auto vfs =
      vnmaki::test::memory_vfs({{"scripts/main.lua", "persistent.copy = persistent.mood"}});
  vnmaki::ScriptRuntime runtime{vfs};
  vnmaki::PersistentStore store{runtime, dir.path()};
  store.load();
  runtime.load_scripts();
  // The first run's value was loaded before the scripts ran, so their top-level code saw it.
  CHECK(persistent_text(runtime, "copy") == "seen");
}

TEST_CASE("persistent: flush writes only a change") {
  const TempDir dir{"onchange"};
  Game game{kStory, dir.path()};
  const std::filesystem::path file = dir.path() / vnmaki::kPersistentFileName;
  game.persistent.flush();
  CHECK_FALSE(std::filesystem::exists(file));

  game.persistent.mark_seen({.label = "start", .step = 1});
  game.persistent.flush();
  CHECK(std::filesystem::exists(file));
}

TEST_CASE("persistent: a damaged file is reported and the game starts empty") {
  const TempDir dir{"damaged"};
  std::filesystem::create_directories(dir.path());
  {
    std::FILE* file = std::fopen((dir.path() / vnmaki::kPersistentFileName).string().c_str(), "wb");
    REQUIRE(file != nullptr);
    std::fputs("not messagepack", file);
    std::fclose(file);
  }
  Game game{kStory, dir.path()};
  REQUIRE(game.warnings.size() == 1);
  CHECK(game.warnings.front().starts_with("persistent:"));
  CHECK(game.runtime.state() == RunState::kWaiting);
}

namespace {

void write_text(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream{path, std::ios::binary | std::ios::trunc} << text;
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream stream{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

// The file is kept as .bad and the first flush does not replace it with defaults.
void check_bad_file_kept(const std::filesystem::path& dir, const std::string& original) {
  const std::filesystem::path file = dir / vnmaki::kPersistentFileName;
  std::filesystem::path bad = file;
  bad += ".bad";
  Game game{kStory, dir};
  REQUIRE(game.warnings.size() == 1);
  game.persistent.flush();
  CHECK(read_text(bad) == original);
  CHECK_FALSE(std::filesystem::exists(file));

  game.persistent.mark_seen({.label = "start", .step = 1});  // a real change still saves
  game.persistent.flush();
  CHECK(std::filesystem::exists(file));
  CHECK(read_text(bad) == original);
}

}  // namespace

TEST_CASE("persistent: a damaged file is kept as .bad, not overwritten by the first flush") {
  const TempDir dir{"damaged_kept"};
  const std::string garbage = "not messagepack";
  write_text(dir.path() / vnmaki::kPersistentFileName, garbage);
  check_bad_file_kept(dir.path(), garbage);
}

TEST_CASE("persistent: a file of a newer format is kept as .bad, not overwritten") {
  const TempDir dir{"newer_format"};
  std::vector<std::byte> bytes = vnmaki::encode_persistent({});
  constexpr std::size_t kFormatValueAt = 8;  // map header, "format" (1 + 6 bytes), then the number
  REQUIRE(bytes.at(kFormatValueAt) == std::byte{vnmaki::kPersistentFormatVersion});
  bytes.at(kFormatValueAt) = std::byte{vnmaki::kPersistentFormatVersion + 1};
  std::string text;
  for (const std::byte b : bytes) {
    text.push_back(static_cast<char>(b));
  }
  write_text(dir.path() / vnmaki::kPersistentFileName, text);
  check_bad_file_kept(dir.path(), text);
}

TEST_CASE("persistent: a load does not change it") {
  const TempDir dir{"load"};
  constexpr auto kScript = R"(
label("start", function()
  narrate("one")
  persistent.late = "set"
  narrate("two")
end)
)";
  const auto vfs = vnmaki::test::memory_vfs({{"scripts/main.lua", kScript}});
  vnmaki::ScriptRuntime runtime{vfs};
  runtime.load_scripts();
  vnmaki::Saves saves{runtime, {.directory = dir.path() / "saves"}};
  runtime.start("start");
  REQUIRE(saves.save("1").empty());
  runtime.advance();  // sets persistent.late
  REQUIRE(persistent_text(runtime, "late") == "set");

  REQUIRE(saves.load("1").empty());

  CHECK(std::get<vnmaki::NarrateRequest>(*runtime.request()).text == "one");
  CHECK(persistent_text(runtime, "late") == "set");
}

TEST_CASE("seen: an edited line is unseen, an unchanged one is seen") {
  const TempDir dir{"edited"};
  {
    Game game{kStory, dir.path()};
    game.read_line();  // one
    game.read_line();  // two
    CHECK(game.persistent.seen_count() == kTwoLines);
  }
  Game edited{kEditedStory, dir.path()};
  // The label's source changed, so none of its steps are seen, not even the unchanged one.
  CHECK_FALSE(edited.persistent.seen({.label = "start", .step = 1}));
  CHECK_FALSE(edited.persistent.seen({.label = "start", .step = kTwoLines}));

  Game same{kStory, dir.path()};
  CHECK(same.persistent.seen({.label = "start", .step = 1}));
  CHECK(same.persistent.seen({.label = "start", .step = kTwoLines}));
  CHECK_FALSE(same.persistent.seen({.label = "start", .step = kThreeLines}));
}

TEST_CASE("skip seen: goes through seen lines and stops at a menu") {
  const TempDir dir{"skip_seen"};
  {
    Game first{kStory, dir.path()};
    first.read_line();  // one
    first.read_line();  // two
    first.read_line();  // three; the menu shows
  }
  // A second playthrough. "one", "two" and "three" are seen.
  Game game{kStory, dir.path(), {.skip_interval = kSkipInterval}};
  game.playback->set_skip(SkipMode::kSeen);
  game.tick_while_skipping(SkipMode::kSeen);

  CHECK(game.playback->skip() == SkipMode::kOff);
  CHECK_FALSE(game.presenter->line().has_value());
  CHECK(std::holds_alternative<vnmaki::MenuRequest>(*game.runtime.request()));
  CHECK(game.runtime.position().step == kThreeLines + 1);
}

TEST_CASE("skip seen: stops at the first unseen line, which is then seen") {
  const TempDir dir{"skip_unseen"};
  {
    Game first{kStory, dir.path()};
    first.read_line();  // one; "two" shows but is not seen yet
  }
  Game game{kStory, dir.path(), {.skip_interval = kSkipInterval}};
  game.playback->set_skip(SkipMode::kSeen);
  game.tick_while_skipping(SkipMode::kSeen);

  CHECK(game.playback->skip() == SkipMode::kOff);
  CHECK(game.shown() == "two");
  CHECK(game.persistent.seen({.label = "start", .step = kTwoLines}));
}

TEST_CASE("skip all: reads unseen lines and stops at a menu") {
  const TempDir dir{"skip_all"};
  Game game{kStory, dir.path(), {.skip_interval = kSkipInterval}};
  game.playback->set_skip(SkipMode::kAll);
  game.tick_while_skipping(SkipMode::kAll);

  CHECK(game.playback->skip() == SkipMode::kOff);
  REQUIRE(game.runtime.state() == RunState::kWaiting);
  CHECK(std::holds_alternative<vnmaki::MenuRequest>(*game.runtime.request()));
  CHECK(game.persistent.seen({.label = "start", .step = kThreeLines}));

  // The player answers the menu; skip is off, so the next line stays.
  game.runtime.choose(0);
  game.tick(kLongTime);
  CHECK(game.shown() == "four");
  game.tick(kLongTime);
  CHECK(game.shown() == "four");
}

TEST_CASE("skip: ends a wait for tweens through the hook, and goes on") {
  const TempDir dir{"skip_wait"};
  constexpr auto kScript = R"(
label("start", function()
  narrate("before")
  wait_all()
  narrate("after")
  menu { "x" }
end)
)";
  vnmaki::ScriptRuntime* runtime_ptr = nullptr;
  int ended_waits = 0;
  Game game{kScript, dir.path(),
            {.skip_interval = kSkipInterval, .skip_wait = [&] {
               if (std::holds_alternative<vnmaki::WaitRequest>(*runtime_ptr->request())) {
                 ++ended_waits;
                 runtime_ptr->advance();
                 return true;
               }
               return false;
             }}};
  runtime_ptr = &game.runtime;
  game.playback->set_skip(SkipMode::kAll);
  game.tick_while_skipping(SkipMode::kAll);

  CHECK(ended_waits == 1);
  CHECK(std::holds_alternative<vnmaki::MenuRequest>(*game.runtime.request()));
  CHECK(game.playback->skip() == SkipMode::kOff);
}

TEST_CASE("script: skip and auto turn the modes on and off") {
  const TempDir dir{"script"};
  constexpr auto kScript = R"(
label("start", function()
  skip("all")
  auto(true)
  narrate("a")
  skip(false)
  auto(false)
  narrate("b")
end)
)";
  Game game{kScript, dir.path()};
  game.tick();  // takes the first two calls; skip then moves on to "b"
  CHECK(game.playback->skip() == SkipMode::kAll);
  CHECK(game.playback->automatic());
  CHECK(game.shown() == "b");
  game.tick();  // takes the last two
  CHECK(game.playback->skip() == SkipMode::kOff);
  CHECK_FALSE(game.playback->automatic());
  CHECK(game.shown() == "b");
}

TEST_CASE("script: bad arguments of skip, auto and auto_multiplier are errors") {
  for (const std::string call : {"skip('fast')", "auto('yes')", "skip(3)"}) {
    const auto vfs = vnmaki::test::memory_vfs(
        {{"scripts/main.lua", "label('start', function() " + call + " end)"}});
    vnmaki::ScriptRuntime runtime{vfs};
    runtime.load_scripts();
    runtime.start("start");
    CHECK(runtime.state() == RunState::kFailed);
  }
  const auto vfs = vnmaki::test::memory_vfs(
      {{"scripts/main.lua", "character { name = 'A', auto_multiplier = 'x' }"}});
  vnmaki::ScriptRuntime runtime{vfs};
  CHECK_THROWS_AS(runtime.load_scripts(), vnmaki::ScriptError);
}

TEST_CASE("auto: waits a base time plus a time for each glyph") {
  const TempDir dir{"auto"};
  Game game{kStory, dir.path(),
            {.auto_base_delay = kBaseDelay, .auto_seconds_per_glyph = kGlyphDelay}};
  game.playback->set_auto(true);
  CHECK(game.playback->auto_delay() == doctest::Approx(kBaseDelay + (kGlyphDelay * kThreeLines)));
  CHECK(game.ticks_until_next_line() == kTicksForOne);
  CHECK(game.shown() == "two");
}

TEST_CASE("auto: the delay is exact, and a character's multiplier scales the glyph part") {
  const TempDir dir{"auto_exact"};
  Game game{kHelloStory, dir.path(),
            {.auto_base_delay = kBaseDelay, .auto_seconds_per_glyph = kGlyphDelay}};
  game.playback->set_auto(true);
  REQUIRE(game.presenter->glyph_count() == kHelloLength);
  CHECK(game.playback->auto_delay() == doctest::Approx(kHelloDelay));

  // 4 ticks of typing, then the wait: it must not end one tick early.
  CHECK(game.ticks_until_next_line() == kTicksUntilAdvance);
  REQUIRE(game.presenter->line().has_value());
  CHECK(game.presenter->line()->speaker == "S");
  CHECK(game.presenter->line()->auto_multiplier == doctest::Approx(kMultiplier));
  CHECK(game.playback->auto_delay() == doctest::Approx(kSlowHelloDelay));
  CHECK(game.ticks_until_next_line() == kTicksUntilSlowAdvance);
  CHECK(game.shown() == "end");
}

TEST_CASE("auto: waits while a voice plays, then only the base delay") {
  const TempDir dir{"auto_voice"};
  bool voice = true;
  Game game{kHelloStory, dir.path(),
            {.auto_base_delay = kBaseDelay,
             .auto_seconds_per_glyph = kGlyphDelay,
             .voice_playing = [&voice] { return voice; }}};
  game.playback->set_auto(true);
  for (std::size_t n = 0; n < kMaxTicks; ++n) {
    game.tick();  // far more than the 1.625 s the line needs without a voice
  }
  CHECK(game.shown() == "Hello");
  CHECK(game.presenter->line()->speaker == "F");

  voice = false;
  CHECK(game.ticks_until_next_line() == kTicksAfterVoice);
}

TEST_CASE("auto: off does nothing") {
  const TempDir dir{"auto_off"};
  Game game{kStory, dir.path()};
  for (std::size_t n = 0; n < kMaxTicks; ++n) {
    game.tick(kLongTime);
  }
  CHECK(game.shown() == "one");
  CHECK_FALSE(game.playback->automatic());
}

TEST_CASE("indicator: shows the text of the active modes and hides when both are off") {
  using vnmaki::kPlaybackScreenName;
  CHECK(vnmaki::playback_indicator_text(SkipMode::kOff, false).empty());
  CHECK(vnmaki::playback_indicator_text(SkipMode::kOff, true) == "AUTO");
  CHECK(vnmaki::playback_indicator_text(SkipMode::kSeen, false) == "SKIP");
  CHECK(vnmaki::playback_indicator_text(SkipMode::kAll, false) == "SKIP ALL");
  CHECK(vnmaki::playback_indicator_text(SkipMode::kAll, true) == "AUTO SKIP ALL");

  constexpr vnmaki::Size kCanvas{.width = 1280, .height = 720};
  vnmaki::UiHost host{kCanvas};
  vnmaki::PlaybackIndicator indicator{host};
  indicator.sync(SkipMode::kOff, false);
  CHECK_FALSE(host.is_shown(kPlaybackScreenName));
  indicator.sync(SkipMode::kSeen, false);
  CHECK(host.is_shown(kPlaybackScreenName));
  CHECK_FALSE(host.has_modal());
  indicator.sync(SkipMode::kOff, true);
  CHECK(host.is_shown(kPlaybackScreenName));
  indicator.sync(SkipMode::kOff, false);
  CHECK_FALSE(host.is_shown(kPlaybackScreenName));
}

TEST_CASE("headless: quick save and persistent work in a run") {
  const TempDir dir{"headless"};
  constexpr auto kScript = R"(
label("start", function()
  persistent.reached = "yes"
  narrate("a")
  quick_save()
  narrate("b")
end)
)";
  const auto vfs = vnmaki::test::memory_vfs({{"scripts/main.lua", kScript}});
  vnmaki::HeadlessOptions options;
  options.saves = vnmaki::SavesOptions{.directory = dir.path()};
  const std::vector<vnmaki::ScriptInput> inputs(kTwoLines, vnmaki::AdvanceInput{});

  const vnmaki::HeadlessResult result = vnmaki::run_headless(vfs, inputs, options);

  CHECK(result.outcome == vnmaki::HeadlessOutcome::kFinished);
  for (const std::string& line : result.transcript) {
    CHECK_FALSE(line.starts_with("warning"));
  }
  CHECK(std::filesystem::exists(dir.path() / "quick.vnsave"));
  CHECK(std::filesystem::exists(dir.path() / vnmaki::kPersistentFileName));
}

TEST_CASE("headless: a load request in the script goes back to the saved line") {
  const TempDir dir{"headless_load"};
  constexpr auto kScript = R"(
label("start", function()
  narrate("a")
  quick_save()
  narrate("b")
  quick_load()
  narrate("c")
end)
)";
  const auto vfs = vnmaki::test::memory_vfs({{"scripts/main.lua", kScript}});
  vnmaki::HeadlessOptions options;
  options.saves = vnmaki::SavesOptions{.directory = dir.path()};
  const std::vector<vnmaki::ScriptInput> inputs(kThreeLines, vnmaki::AdvanceInput{});

  const vnmaki::HeadlessResult result = vnmaki::run_headless(vfs, inputs, options);

  // The save is made at "b", the first yield point after the call. Leaving "b" queues a load,
  // which is done at "c": the story is at "b" again, and so on until the inputs run out.
  std::size_t b_lines = 0;
  for (const std::string& line : result.transcript) {
    b_lines += line == "narrate b" ? 1 : 0;
  }
  CHECK(b_lines == kThreeLines);
  CHECK(result.outcome == vnmaki::HeadlessOutcome::kOutOfInput);
}
