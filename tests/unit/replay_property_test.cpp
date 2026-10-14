// The replay property (docs/SPEC.md section 8): for every yield point of a
// story, restoring the label-entry snapshot and replaying the input log to
// that step gives the same store, scene state, position and request as live
// play did, and the story goes on from there as live play did.
//
// The stories come from tools/generators/replay_fixtures.py. Live play is the
// oracle: it never replays.

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/runtime/script_runtime.hpp"
#include "engine/vfs/vfs.hpp"

namespace {

using vnmaki::InputResult;
using vnmaki::LabelSnapshot;
using vnmaki::RunState;
using vnmaki::ScriptRuntime;
using Bytes = std::vector<std::byte>;

constexpr std::size_t kCases = 8;
// Each story is played with two different sets of menu answers.
constexpr std::size_t kAnswerSets = 2;
// A story that has not ended by then is a generator bug.
constexpr std::size_t kMaxStops = 400;

// A scene whose state is one byte string; scripts cannot change it yet
// (ticket 09 adds the calls), so it changes only when a test sets it.
class StageScene final : public vnmaki::SceneState {
 public:
  explicit StageScene(std::string_view text) { set(text); }

  [[nodiscard]] Bytes capture() const override { return state; }
  void restore(std::span<const std::byte> bytes) override {
    state.assign(bytes.begin(), bytes.end());
  }
  void set(std::string_view text) {
    state.clear();
    state.reserve(text.size());
    for (const char c : text) {
      state.push_back(static_cast<std::byte>(c));
    }
  }

  Bytes state;
};

// Everything that must be the same after a replay.
struct Stop {
  vnmaki::ScriptPosition position;
  vnmaki::BlockingRequest request;
  vnmaki::StoreSnapshot store;
  Bytes scene;
  bool in_menu = false;

  friend bool operator==(const Stop&, const Stop&) = default;
};

// One runtime over a generated story, with its scene.
struct Play {
  explicit Play(const vnmaki::Vfs& vfs) : runtime(vfs, {.scene_state = &scene}) {
    runtime.load_scripts();
  }

  [[nodiscard]] bool waiting() const { return runtime.state() == RunState::kWaiting; }

  [[nodiscard]] Stop stop() const {
    return Stop{.position = runtime.position(),
                .request = *runtime.request(),
                .store = runtime.store_snapshot(),
                .scene = scene.state,
                .in_menu = std::holds_alternative<vnmaki::MenuRequest>(*runtime.request())};
  }

  // Answers the yield point number `stop_number` of the story. The answer
  // depends only on that number and the answer set, so every play of the
  // story gives the same answers.
  void answer(std::size_t stop_number, std::size_t answer_set) {
    if (const auto* menu = std::get_if<vnmaki::MenuRequest>(runtime.request())) {
      runtime.choose((stop_number + answer_set) % menu->options.size());
    } else {
      runtime.advance();
    }
  }

  StageScene scene{"classroom"};
  ScriptRuntime runtime;
};

std::vector<Stop> play_live(const vnmaki::Vfs& vfs, std::size_t answer_set) {
  Play play{vfs};
  play.runtime.start("start");
  std::vector<Stop> stops;
  while (play.waiting() && stops.size() < kMaxStops) {
    stops.push_back(play.stop());
    play.answer(stops.size() - 1, answer_set);
  }
  REQUIRE_MESSAGE(play.runtime.state() == RunState::kFinished, play.runtime.error());
  return stops;
}

}  // namespace

TEST_CASE("replay: replaying to any step of a generated story matches live play") {
  std::size_t menus = 0;
  std::size_t stops_in_called_labels = 0;
  for (std::size_t story = 0; story < kCases; ++story) {
    for (std::size_t answer_set = 0; answer_set < kAnswerSets; ++answer_set) {
      CAPTURE(story);
      CAPTURE(answer_set);
      const std::string name = "case_0" + std::to_string(story);  // kCases is below 10
      vnmaki::Vfs vfs;
      REQUIRE(vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_FIXTURES_DIR} /
                                                     "replay" / name)));
      const std::vector<Stop> live = play_live(vfs, answer_set);
      REQUIRE(live.size() >= 3);

      for (std::size_t target = 0; target < live.size(); ++target) {
        CAPTURE(target);
        const Stop& expected = live[target];
        menus += expected.in_menu ? 1 : 0;
        stops_in_called_labels +=
            expected.position.label == "aside" || expected.position.label == "detour" ? 1 : 0;

        Play play{vfs};
        play.runtime.start("start");
        for (std::size_t stop = 0; stop < target; ++stop) {
          play.answer(stop, answer_set);
        }
        REQUIRE(play.stop() == expected);
        const LabelSnapshot snapshot = *play.runtime.label_snapshot();
        const std::vector<InputResult> log{play.runtime.input_log().begin(),
                                           play.runtime.input_log().end()};

        // A runtime that never played the story, as when a save is loaded.
        Play loaded{vfs};
        loaded.scene.set("somewhere else");
        loaded.runtime.replay(snapshot, log, expected.position.step);
        REQUIRE_MESSAGE(loaded.waiting(), loaded.runtime.error());
        CHECK(loaded.stop() == expected);
        CHECK(loaded.runtime.replaying() == false);

        // The same runtime, as in a rollback: the labels below the replayed
        // one stay, so the story then goes on exactly as live play did.
        play.scene.set("somewhere else");
        play.runtime.replay(snapshot, log, expected.position.step);
        REQUIRE_MESSAGE(play.waiting(), play.runtime.error());
        CHECK(play.stop() == expected);
        std::size_t stop = target;
        while (play.waiting() && stop + 1 < live.size()) {
          play.answer(stop, answer_set);
          ++stop;
          REQUIRE(play.waiting());
          REQUIRE(play.stop() == live[stop]);
        }
        play.answer(stop, answer_set);
        CHECK(play.runtime.state() == RunState::kFinished);
      }
    }
  }
  // The generated stories did exercise what the property is about.
  CHECK(menus > kCases);
  CHECK(stops_in_called_labels > kCases);
}
