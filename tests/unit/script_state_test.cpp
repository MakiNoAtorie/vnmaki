// State and replay (docs/SPEC.md section 8) through ScriptRuntime's public
// interface: the store, the RNG, the position, the label-entry snapshot, the
// input log, the label source hash and replay to a step. Expected values are
// written out by hand from the script text.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/runtime/script_runtime.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::InputResult;
using vnmaki::LabelSnapshot;
using vnmaki::MenuChoice;
using vnmaki::MenuRequest;
using vnmaki::RunState;
using vnmaki::ScriptPosition;
using vnmaki::ScriptRuntimeOptions;
using vnmaki::StoreSnapshot;
using vnmaki::StoreTable;
using vnmaki::StoreTableRef;
using vnmaki::StoreValue;
using Lines = std::vector<std::string>;
using Bytes = std::vector<std::byte>;

// Writes down everything the runtime reports.
class Recorder final : public vnmaki::ScriptObserver {
 public:
  void on_label_entered(std::string_view label) override {
    events.push_back("label " + std::string{label});
  }
  void on_label_resumed(std::string_view label) override {
    events.push_back("resume " + std::string{label});
  }
  void on_blocked(const vnmaki::BlockingRequest& /*request*/,
                  const ScriptPosition& position) override {
    events.push_back("stop " + position.label + " " + std::to_string(position.step));
  }
  void on_print(std::string_view text) override { events.push_back("print " + std::string{text}); }
  void on_warning(std::string_view message) override {
    events.push_back("warning " + std::string{message});
  }

  Lines events;
};

// A scene whose whole state is a few bytes. It notes whether the replay flag
// was set each time the runtime asked for its state.
class FakeScene final : public vnmaki::SceneState {
 public:
  [[nodiscard]] Bytes capture() const override {
    if (runtime != nullptr) {
      replaying_at_capture.push_back(runtime->replaying());
    }
    return state;
  }
  void restore(std::span<const std::byte> bytes) override {
    state.assign(bytes.begin(), bytes.end());
  }

  Bytes state;
  const vnmaki::ScriptRuntime* runtime = nullptr;
  mutable std::vector<bool> replaying_at_capture;
};

ScriptRuntimeOptions observed(ScriptRuntimeOptions options, Recorder& recorder) {
  options.observer = &recorder;
  return options;
}

// One script, loaded and ready to start.
struct Game {
  explicit Game(const std::string& source, const ScriptRuntimeOptions& options = {})
      : vfs(vnmaki::test::memory_vfs({{"scripts/main.lua", source}})),
        runtime(vfs, observed(options, recorder)) {
    runtime.load_scripts();
  }

  [[nodiscard]] Lines printed() const {
    Lines lines;
    for (const std::string& event : recorder.events) {
      if (event.starts_with("print ")) {
        lines.push_back(event.substr(std::string_view{"print "}.size()));
      }
    }
    return lines;
  }

  vnmaki::Vfs vfs;
  Recorder recorder;
  vnmaki::ScriptRuntime runtime;
};

StoreValue string_value(std::string value) { return {std::move(value)}; }
StoreValue integer_value(std::int64_t value) { return {value}; }

Bytes to_bytes(std::string_view characters) {
  Bytes result;
  result.reserve(characters.size());
  for (const char c : characters) {
    result.push_back(static_cast<std::byte>(c));
  }
  return result;
}

constexpr std::int64_t kSnapshotSeed = 7;
constexpr double kHalf = 0.5;

// A label calls another, which draws a number and asks a menu. The caller
// keeps a local across the call.
constexpr std::string_view kCallStory = R"(
label("start", function()
  local before = rand(1, 1000)
  call("aside")
  store.sum = before + store.inner
  narrate("back in start")
end)
label("aside", function()
  store.inner = rand(1, 1000)
  narrate("aside one")
  store.inner = store.inner + menu({ "a", "b" })
  narrate("aside three")
end)
)";

// A label with lines, a menu, random draws and a local that lives across
// yield points. Its steps: 1 "one", 2 the menu, 3 "three", 4 "four".
constexpr std::string_view kStory = R"(
label("start", function()
  store.rolls = {}
  local first = rand(1, 1000)
  narrate("one")
  store.rolls[1] = first
  local picked = menu({ "left", "right" })
  print("picked " .. picked)
  store.picked = picked
  store.rolls[2] = rand(1, 1000)
  narrate("three")
  store.rolls[3] = first + rand(1, 1000)
  narrate("four")
end)
)";

}  // namespace

TEST_CASE("state: store carries values between labels and cannot be replaced") {
  Game game{R"(
store.affection = 1
label("start", function()
  store.affection = store.affection + 1
  jump("next")
end)
label("next", function()
  print(store.affection)
  print(pcall(function() store = {} end))
  print(store.affection)
end)
)"};

  game.runtime.start("start");

  REQUIRE(game.runtime.state() == RunState::kFinished);
  CHECK(game.printed() == Lines{"2",
                                "false\tscripts/main.lua:9: 'store' is owned by the engine and "
                                "cannot be replaced (change its fields)",
                                "2"});
}

TEST_CASE("state: rand draws the splitmix64 sequence of the seed") {
  // The first outputs of splitmix64 seeded with 1234567 are
  // 6457827717110365317 and 3203168211198807973 (Vigna's reference code).
  // rand(0) gives the 64 random bits as an integer offset by math.mininteger.
  constexpr std::uint64_t kSeed = 1234567;
  ScriptRuntimeOptions options;
  options.random_seed = kSeed;
  Game game{"label('start', function() print(rand(0), math.random(0)) end)", options};

  game.runtime.start("start");

  CHECK(game.printed() == Lines{"-2765544319744410491\t-6020203825655967835"});
}

TEST_CASE("state: the RNG state is a field of store") {
  Game game{R"(
label("start", function()
  local before = store.__rng
  local first = rand(1, 1000000)
  print(math.type(before), before ~= store.__rng)
  store.__rng = before
  print(rand(1, 1000000) == first, rand == math.random)
end)
)"};

  game.runtime.start("start");

  CHECK(game.printed() == Lines{"integer\ttrue", "true\ttrue"});
}

TEST_CASE("state: the position is the label and the count of its yield points") {
  Game game{R"(
label("start", function()
  narrate("one")
  menu({ "a", "b" })
  jump("next")
end)
label("next", function()
  say("A", "first of next")
end)
)"};

  CHECK(game.runtime.position() == ScriptPosition{});
  game.runtime.start("start");
  CHECK(game.runtime.position() == ScriptPosition{.label = "start", .step = 1});
  game.runtime.advance();
  CHECK(game.runtime.position() == ScriptPosition{.label = "start", .step = 2});
  game.runtime.choose(0);
  CHECK(game.runtime.position() == ScriptPosition{.label = "next", .step = 1});
  game.runtime.advance();
  CHECK(game.runtime.state() == RunState::kFinished);
  CHECK(game.runtime.position() == ScriptPosition{});
}

TEST_CASE("state: a menu waits for a choice and returns its number") {
  Game game{R"(
label("start", function()
  print(menu({ "Of course I did.", "I forgot my bag." }))
  narrate("after")
end)
)"};
  game.runtime.start("start");

  REQUIRE(game.runtime.state() == RunState::kWaiting);
  REQUIRE(game.runtime.request() != nullptr);
  CHECK(*game.runtime.request() ==
        vnmaki::BlockingRequest{MenuRequest{.options = {"Of course I did.", "I forgot my bag."}}});

  SUBCASE("advance does not answer it") {
    game.runtime.advance();
    CHECK(game.runtime.position() == ScriptPosition{.label = "start", .step = 1});
    CHECK(game.printed().empty());
  }
  SUBCASE("choose resumes the label with the option's number") {
    game.runtime.choose(1);
    CHECK(game.printed() == Lines{"2"});
    CHECK(game.runtime.position() == ScriptPosition{.label = "start", .step = 2});
  }
  SUBCASE("an option the menu does not have is refused") {
    CHECK_THROWS_AS(game.runtime.choose(2), std::out_of_range);
    CHECK(game.runtime.position() == ScriptPosition{.label = "start", .step = 1});
  }
  SUBCASE("choose answers only menus") {
    game.runtime.choose(0);
    game.runtime.choose(0);
    CHECK(game.runtime.position() == ScriptPosition{.label = "start", .step = 2});
  }
}

TEST_CASE("state: a menu needs a list of strings") {
  Game game{R"(
label("start", function()
  print(pcall(menu, {}))
  print(pcall(menu, { "a", 2 }))
  print(pcall(menu, "a"))
end)
)"};

  game.runtime.start("start");

  CHECK(game.printed() == Lines{"false\tmenu() needs at least one option",
                                "false\tbad option #2 to 'menu' (string expected, got number)",
                                "false\tbad argument #1 to 'menu' (table expected, got string)"});
}

TEST_CASE("state: the input log holds the choices made since the label was entered") {
  Game game{R"(
label("start", function()
  menu({ "a", "b", "c" })
  narrate("between")
  call("aside")
  menu({ "a", "b" })
  jump("next")
end)
label("aside", function()
  menu({ "x", "y" })
  narrate("in aside")
end)
label("next", function()
  narrate("in next")
end)
)"};
  const auto log = [&game] {
    const std::span<const InputResult> inputs = game.runtime.input_log();
    return std::vector<InputResult>{inputs.begin(), inputs.end()};
  };
  using Log = std::vector<InputResult>;

  CHECK(log().empty());
  game.runtime.start("start");
  CHECK(log().empty());
  game.runtime.choose(2);
  CHECK(log() == Log{MenuChoice{.index = 2}});
  game.runtime.advance();  // now at the menu of "aside"
  CHECK(log().empty());
  game.runtime.choose(1);
  CHECK(log() == Log{MenuChoice{.index = 1}});
  game.runtime.advance();  // back in "start", whose log covers the call
  CHECK(log() == Log{MenuChoice{.index = 2}, MenuChoice{.index = 1}});
  game.runtime.choose(0);
  CHECK(game.runtime.position() == ScriptPosition{.label = "next", .step = 1});
  CHECK(log().empty());
}

TEST_CASE("state: entering a label snapshots the store") {
  ScriptRuntimeOptions options;
  options.random_seed = static_cast<std::uint64_t>(kSnapshotSeed);
  Game game{R"(
store.a = 1
store.name = "x"
store.t = { true, half = 0.5 }
store.same = store.t
label("start", function()
  store.a = 2
  narrate("one")
end)
)",
            options};
  const auto store_with_a = [](std::int64_t a) {
    return StoreSnapshot{
        .tables = {
            StoreTable{.entries = {{string_value("__rng"), integer_value(kSnapshotSeed)},
                                   {string_value("a"), integer_value(a)},
                                   {string_value("name"), string_value("x")},
                                   {string_value("same"), StoreTableRef{.index = 1}},
                                   {string_value("t"), StoreTableRef{.index = 1}}}},
            StoreTable{
                .entries = {{integer_value(1), StoreValue{true}}, {string_value("half"), kHalf}}},
        }};
  };

  CHECK(game.runtime.label_snapshot() == nullptr);
  game.runtime.start("start");

  REQUIRE(game.runtime.label_snapshot() != nullptr);
  CHECK(game.runtime.label_snapshot()->label == "start");
  CHECK(game.runtime.label_snapshot()->store == store_with_a(1));
  CHECK(game.runtime.store_snapshot() == store_with_a(2));
}

TEST_CASE("state: a store that cannot be snapshotted fails the next label entry") {
  Game game{R"(
label("start", function()
  store.callback = print
  jump("next")
end)
label("next", function() end)
)"};

  game.runtime.start("start");

  CHECK(game.runtime.state() == RunState::kFailed);
  CHECK(game.runtime.error() ==
        "entering label 'next': store holds a function value (field 'callback'); it keeps only "
        "booleans, numbers, strings and tables");
}

TEST_CASE("state: entering a label snapshots the scene state") {
  FakeScene scene;
  scene.state = to_bytes("classroom");
  ScriptRuntimeOptions options;
  options.scene_state = &scene;
  Game game{R"(
label("start", function()
  narrate("one")
  jump("next")
end)
label("next", function()
  narrate("two")
end)
)",
            options};

  game.runtime.start("start");
  scene.state = to_bytes("rooftop");
  REQUIRE(game.runtime.label_snapshot() != nullptr);
  CHECK(game.runtime.label_snapshot()->scene == to_bytes("classroom"));

  game.runtime.advance();
  REQUIRE(game.runtime.label_snapshot() != nullptr);
  CHECK(game.runtime.label_snapshot()->label == "next");
  CHECK(game.runtime.label_snapshot()->scene == to_bytes("rooftop"));
}

TEST_CASE("state: a label's source hash follows its own text only") {
  const auto hashes = [](const std::string& source) {
    Game game{source};
    return std::pair{game.runtime.label_source_hash("a"), game.runtime.label_source_hash("b")};
  };
  const std::string label_a = "label(\"a\", function()\n  narrate(\"one\")\nend)\n";
  const std::string label_b = "label(\"b\", function()\n  narrate(\"two\")\nend)\n";
  const auto [a, b] = hashes(label_a + label_b);

  CHECK(a.has_value());
  CHECK(b.has_value());
  CHECK(a != b);
  // FNV-1a 64 of the label's three lines, computed with Python:
  //   h = 0xcbf29ce484222325
  //   for c in b'label("a", function()\n  narrate("one")\nend)\n':
  //       h = ((h ^ c) * 0x100000001b3) % 2**64
  CHECK(a == std::optional<std::uint64_t>{0x6A525E5D789C3845ULL});

  SUBCASE("moving the label and changing other labels keeps it") {
    const auto [moved_a, other_b] =
        hashes("-- a new comment\n\nlabel(\"b\", function()\n  narrate(\"2\")\nend)\n" + label_a);
    CHECK(moved_a == a);
    CHECK(other_b != b);
  }
  SUBCASE("it does not depend on the line ends") {
    const auto [crlf_a, crlf_b] = hashes(
        "label(\"a\", function()\r\n  narrate(\"one\")\r\nend)\r\n"
        "label(\"b\", function()\r\n  narrate(\"two\")\r\nend)\r\n");
    CHECK(crlf_a == a);
    CHECK(crlf_b == b);
  }
  SUBCASE("an unknown label has none") {
    Game game{label_a};
    CHECK(game.runtime.label_source_hash("b") == std::nullopt);
  }
}

TEST_CASE("replay: replaying to a step lands where live play was") {
  Game game{std::string{kStory}};
  game.runtime.start("start");
  game.runtime.advance();
  game.runtime.choose(1);  // step 3
  const StoreSnapshot store_at_3 = game.runtime.store_snapshot();
  const vnmaki::BlockingRequest request_at_3 = *game.runtime.request();
  game.runtime.advance();  // step 4
  const StoreSnapshot store_at_4 = game.runtime.store_snapshot();
  REQUIRE(game.runtime.label_snapshot() != nullptr);

  // The arguments belong to the label that the replay replaces.
  game.runtime.replay(*game.runtime.label_snapshot(), game.runtime.input_log(), 3);

  REQUIRE_MESSAGE(game.runtime.state() == RunState::kWaiting, game.runtime.error());
  CHECK(game.runtime.position() == ScriptPosition{.label = "start", .step = 3});
  CHECK(*game.runtime.request() == request_at_3);
  CHECK(game.runtime.store_snapshot() == store_at_3);
  CHECK(game.runtime.input_log().size() == 1);

  // The label's locals are back too: step 4 adds `first` to a new draw.
  game.runtime.advance();
  CHECK(game.runtime.position() == ScriptPosition{.label = "start", .step = 4});
  CHECK(game.runtime.store_snapshot() == store_at_4);
  game.runtime.advance();
  CHECK(game.runtime.state() == RunState::kFinished);
}

TEST_CASE("replay: a fresh runtime reaches the same state from a snapshot and a log") {
  Game live{std::string{kStory}};
  live.runtime.start("start");
  live.runtime.advance();
  live.runtime.choose(0);
  REQUIRE(live.runtime.label_snapshot() != nullptr);

  Game loaded{std::string{kStory}};
  loaded.runtime.replay(*live.runtime.label_snapshot(), live.runtime.input_log(), 3);

  REQUIRE_MESSAGE(loaded.runtime.state() == RunState::kWaiting, loaded.runtime.error());
  CHECK(loaded.runtime.position() == live.runtime.position());
  CHECK(*loaded.runtime.request() == *live.runtime.request());
  CHECK(loaded.runtime.store_snapshot() == live.runtime.store_snapshot());
  CHECK(loaded.recorder.events == Lines{"stop start 3"});
}

TEST_CASE("replay: nothing is reported until the target step") {
  FakeScene scene;
  ScriptRuntimeOptions options;
  options.scene_state = &scene;
  Game game{R"(
label("start", function()
  print("before one")
  narrate("one")
  leaked = true
  call("aside")
  print("before two")
  narrate("two")
  print("after two")
  narrate("three")
end)
label("aside", function()
  narrate("in aside")
end)
)",
            options};
  scene.runtime = &game.runtime;
  game.runtime.start("start");
  game.runtime.advance();
  game.runtime.advance();
  game.runtime.advance();  // step 3 of "start"
  REQUIRE(game.runtime.position() == ScriptPosition{.label = "start", .step = 3});
  CHECK(game.runtime.replaying() == false);
  game.recorder.events.clear();
  scene.replaying_at_capture.clear();

  game.runtime.replay(*game.runtime.label_snapshot(), game.runtime.input_log(), 2);

  CHECK(game.recorder.events == Lines{"stop start 2"});
  // The replay entered "aside", and the scene could see the flag then.
  CHECK(scene.replaying_at_capture == std::vector<bool>{true});
  CHECK(game.runtime.replaying() == false);

  game.runtime.advance();
  CHECK(game.recorder.events == Lines{"stop start 2", "print after two", "stop start 3"});
}

TEST_CASE("replay: the scene state of the snapshot is restored") {
  FakeScene scene;
  scene.state = to_bytes("classroom");
  ScriptRuntimeOptions options;
  options.scene_state = &scene;
  Game game{"label('start', function() narrate('one') narrate('two') end)", options};
  game.runtime.start("start");
  game.runtime.advance();
  scene.state = to_bytes("rooftop");

  game.runtime.replay(*game.runtime.label_snapshot(), game.runtime.input_log(), 1);

  CHECK(scene.state == to_bytes("classroom"));
  CHECK(game.runtime.position() == ScriptPosition{.label = "start", .step = 1});
}

TEST_CASE("replay: a called label is replayed and its caller goes on afterwards") {
  // Live: through "aside" (choosing option 2) and back into "start".
  Game live{std::string{kCallStory}};
  live.runtime.start("start");
  live.runtime.advance();
  live.runtime.choose(1);
  live.runtime.advance();
  REQUIRE(live.runtime.position() == ScriptPosition{.label = "start", .step = 1});

  Game game{std::string{kCallStory}};
  game.runtime.start("start");
  game.runtime.advance();
  game.runtime.choose(0);
  REQUIRE(game.runtime.position() == ScriptPosition{.label = "aside", .step = 3});

  // Back to the first line of "aside", then the other option.
  game.runtime.replay(*game.runtime.label_snapshot(), game.runtime.input_log(), 1);
  CHECK(game.runtime.position() == ScriptPosition{.label = "aside", .step = 1});
  game.runtime.advance();
  game.runtime.choose(1);
  game.runtime.advance();

  // "start" still had its local `before`, so the sums agree.
  CHECK(game.runtime.position() == ScriptPosition{.label = "start", .step = 1});
  CHECK(game.runtime.store_snapshot() == live.runtime.store_snapshot());
  game.runtime.advance();
  CHECK(game.runtime.state() == RunState::kFinished);
}

TEST_CASE("replay: on a runtime that has not started, the label is the outermost one") {
  Game live{std::string{kCallStory}};
  live.runtime.start("start");
  live.runtime.advance();
  live.runtime.choose(1);
  REQUIRE(live.runtime.position() == ScriptPosition{.label = "aside", .step = 3});

  Game loaded{std::string{kCallStory}};
  loaded.runtime.replay(*live.runtime.label_snapshot(), live.runtime.input_log(), 3);

  CHECK(loaded.runtime.position() == ScriptPosition{.label = "aside", .step = 3});
  CHECK(loaded.runtime.store_snapshot() == live.runtime.store_snapshot());
  loaded.runtime.advance();
  CHECK(loaded.runtime.state() == RunState::kFinished);
}

TEST_CASE("replay: step 0 enters the label like a jump") {
  Game game{std::string{kStory}};
  game.runtime.start("start");
  game.runtime.advance();
  game.recorder.events.clear();

  game.runtime.replay(*game.runtime.label_snapshot(), {}, 0);

  CHECK(game.recorder.events == Lines{"label start", "stop start 1"});
}

TEST_CASE("replay: a label that cannot reach the step fails the runtime") {
  Game game{std::string{kStory}};
  game.runtime.start("start");
  game.runtime.advance();
  game.runtime.choose(0);
  const LabelSnapshot snapshot = *game.runtime.label_snapshot();
  const std::vector<InputResult> log{game.runtime.input_log().begin(),
                                     game.runtime.input_log().end()};

  SUBCASE("the label ends first") {
    constexpr std::uint32_t kPastTheEnd = 5;
    game.runtime.replay(snapshot, log, kPastTheEnd);
    CHECK(game.runtime.state() == RunState::kFailed);
    CHECK(game.runtime.error() == "replay: label 'start' ended before step 5");
    CHECK(game.runtime.replaying() == false);
  }
  SUBCASE("the input log has no answer for a menu") {
    game.runtime.replay(snapshot, {}, 3);
    CHECK(game.runtime.state() == RunState::kFailed);
    CHECK(game.runtime.error() ==
          "replay: label 'start', step 2: the input log has no choice for this menu");
  }
  SUBCASE("the logged choice is not an option any more") {
    const std::vector<InputResult> stale{MenuChoice{.index = 2}};
    game.runtime.replay(snapshot, stale, 3);
    CHECK(game.runtime.state() == RunState::kFailed);
    CHECK(game.runtime.error() ==
          "replay: label 'start', step 2: the logged choice is not an option of this menu");
  }
  SUBCASE("the label is unknown") {
    LabelSnapshot unknown = snapshot;
    unknown.label = "nowhere";
    game.runtime.replay(unknown, log, 1);
    CHECK(game.runtime.state() == RunState::kFailed);
    CHECK(game.runtime.error() == "replay: unknown label 'nowhere'");
  }
  SUBCASE("the store snapshot refers to a table it does not have") {
    LabelSnapshot broken = snapshot;
    broken.store.tables.front().entries.emplace_back(string_value("missing"),
                                                     StoreTableRef{.index = 1});
    game.runtime.replay(broken, log, 1);
    CHECK(game.runtime.state() == RunState::kFailed);
    CHECK(game.runtime.error() == "replay: the store snapshot of label 'start' is malformed");
  }
}

// --- CompositeSceneState -----------------------------------------------------------------------

namespace {

// A scene that only takes states that start with the byte it was made with.
class PickyScene final : public vnmaki::SceneState {
 public:
  explicit PickyScene(std::byte first) : first_(first) {}

  [[nodiscard]] Bytes capture() const override { return state; }
  void restore(std::span<const std::byte> bytes) override {
    if (!bytes.empty() && bytes.front() != first_) {
      throw std::invalid_argument{"picky scene: not my state"};
    }
    state.assign(bytes.begin(), bytes.end());
  }

  Bytes state;

 private:
  std::byte first_;
};

constexpr std::byte kA{0xA};
constexpr std::byte kB{0xB};

}  // namespace

TEST_CASE("composite scene state: a snapshot holds every part and gives each its bytes back") {
  PickyScene first{kA};
  PickyScene second{kB};
  vnmaki::CompositeSceneState both{{&first, &second}};
  first.state = Bytes{kA, kA};
  second.state = Bytes{kB};
  const Bytes snapshot = both.capture();

  first.state.clear();
  second.state = Bytes{kB, kB, kB};
  both.restore(snapshot);

  CHECK(first.state == Bytes{kA, kA});
  CHECK(second.state == Bytes{kB});
  CHECK(both.capture() == snapshot);
}

TEST_CASE("composite scene state: a snapshot with no bytes empties every part") {
  PickyScene first{kA};
  PickyScene second{kB};
  vnmaki::CompositeSceneState both{{&first, &second}};
  first.state = Bytes{kA};
  second.state = Bytes{kB};

  both.restore({});

  CHECK(first.state.empty());
  CHECK(second.state.empty());
}

TEST_CASE("composite scene state: a snapshot one part rejects changes no part") {
  PickyScene first{kA};
  PickyScene second{kB};
  vnmaki::CompositeSceneState both{{&first, &second}};
  first.state = Bytes{kA};
  second.state = Bytes{kB};
  const Bytes before = both.capture();

  // The parts the other way round: `first` takes its bytes, then `second` refuses.
  PickyScene other_first{kA};
  PickyScene other_second{kA};
  vnmaki::CompositeSceneState other{{&other_first, &other_second}};
  other_first.state = Bytes{kA, kA, kA};
  other_second.state = Bytes{kA};
  CHECK_THROWS_AS(both.restore(other.capture()), std::invalid_argument);
  CHECK(both.capture() == before);

  Bytes cut = before;
  cut.pop_back();
  CHECK_THROWS_AS(both.restore(cut), std::invalid_argument);
  vnmaki::CompositeSceneState one{{&first}};
  CHECK_THROWS_AS(both.restore(one.capture()), std::invalid_argument);
  CHECK(both.capture() == before);
}
