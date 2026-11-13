// Animation (docs/SPEC.md section 10): easings, the tween timeline, the script's tween and wait
// calls, and the snapshot. Time is a fake clock: the tests pass the elapsed seconds themselves.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/anim/easing.hpp"
#include "engine/anim/timeline.hpp"
#include "engine/anim/wait_presenter.hpp"
#include "engine/core/byte_stream.hpp"
#include "engine/runtime/headless_runner.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/displayable.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::Channel;
using vnmaki::DisplayProperties;
using vnmaki::Ease;
using vnmaki::Scene;
using vnmaki::Timeline;
using vnmaki::TweenNode;
using vnmaki::WaitPresenter;
using Bytes = std::vector<std::byte>;
using Kind = TweenNode::Kind;

constexpr double kEpsilon = 1e-4;
constexpr double kQuarter = 0.25;
constexpr double kHalf = 0.5;
constexpr double kThreeQuarters = 0.75;
constexpr double kEighth = 0.125;
constexpr double kSevenEighths = 0.875;
// The in sine curve at one half is 1 - cos(pi / 4); the out curve is sin(pi / 4).
constexpr double kInSineHalf = 0.29289321881345254;
constexpr double kOutSineHalf = 0.7071067811865476;
// The back curves at one half, with the overshoot 1.70158.
constexpr double kInBackHalf = -0.0876975;
constexpr double kOutBackHalf = 1.0876975;
constexpr double kInBounceHalf = 0.234375;
constexpr double kOutBounceHalf = 0.765625;

// --- Easings -------------------------------------------------------------------------------

struct EaseCase {
  Ease ease = Ease::kLinear;
  double half = 0;
};

constexpr std::array kEaseCases{
    EaseCase{.ease = Ease::kLinear, .half = kHalf},
    EaseCase{.ease = Ease::kInSine, .half = kInSineHalf},
    EaseCase{.ease = Ease::kOutSine, .half = kOutSineHalf},
    EaseCase{.ease = Ease::kInOutSine, .half = kHalf},
    EaseCase{.ease = Ease::kInQuad, .half = kQuarter},
    EaseCase{.ease = Ease::kOutQuad, .half = kThreeQuarters},
    EaseCase{.ease = Ease::kInOutQuad, .half = kHalf},
    EaseCase{.ease = Ease::kInCubic, .half = kEighth},
    EaseCase{.ease = Ease::kOutCubic, .half = kSevenEighths},
    EaseCase{.ease = Ease::kInOutCubic, .half = kHalf},
    EaseCase{.ease = Ease::kInBack, .half = kInBackHalf},
    EaseCase{.ease = Ease::kOutBack, .half = kOutBackHalf},
    EaseCase{.ease = Ease::kInOutBack, .half = kHalf},
    EaseCase{.ease = Ease::kInBounce, .half = kInBounceHalf},
    EaseCase{.ease = Ease::kOutBounce, .half = kOutBounceHalf},
    EaseCase{.ease = Ease::kInOutBounce, .half = kHalf},
};

// --- A timeline over displayables kept in a map --------------------------------------------

constexpr float kFar = 100;
constexpr float kFarther = 300;
constexpr float kMiddle = 50;
constexpr float kTwoHundred = 200;

struct Bench {
  Bench() {
    shown["a"] = DisplayProperties{};
    shown["b"] = DisplayProperties{};
  }

  Timeline::Lookup lookup() {
    return [this](std::string_view tag) -> DisplayProperties* {
      const auto found = shown.find(tag);
      return found == shown.end() ? nullptr : &found->second;
    };
  }
  // Starts `node`; returns why not.
  std::string start(const TweenNode& node, bool instant = false) {
    return timeline.start(node, lookup(), instant);
  }
  // Moves the clock on by `total` seconds in steps of `step`.
  void run(double total, double step) {
    const std::int64_t count = std::llround(total / step);
    for (std::int64_t i = 0; i < count; ++i) {
      timeline.update(step, lookup());
    }
  }
  [[nodiscard]] float x(const std::string& tag = "a") const { return shown.at(tag).position.x; }
  [[nodiscard]] float y(const std::string& tag = "a") const { return shown.at(tag).position.y; }

  std::map<std::string, DisplayProperties, std::less<>> shown;
  Timeline timeline;
};

TweenNode move(const std::string& tag, Channel channel, float to, double duration) {
  TweenNode node;
  node.tag = tag;
  node.to.at(vnmaki::slot(channel)) = to;
  node.duration = duration;
  return node;
}

TweenNode group(Kind kind, std::vector<TweenNode> children) {
  TweenNode node;
  node.kind = kind;
  node.children = std::move(children);
  return node;
}

// --- A game with a scene -------------------------------------------------------------------

constexpr vnmaki::Size kVirtualSize{.width = 1280, .height = 720};
constexpr float kLeftX = 320;
constexpr float kEyeX = 640;
constexpr float kRightX = 960;
constexpr float kEndX = 1024;
constexpr float kMidX = 672;
constexpr float kStageY = 720;

vnmaki::Vfs game_files(const std::string& script) {
  return vnmaki::test::memory_vfs({
      {"scripts/main.lua", script},
      {"images/eileen.png", ""},
      {"images/lucy.png", ""},
  });
}

struct Stage {
  explicit Stage(const std::string& script)
      : vfs(game_files(script)), scene(vfs, kVirtualSize), runtime(vfs, options()) {
    runtime.load_scripts();
    runtime.start("start");
  }

  [[nodiscard]] vnmaki::ScriptRuntimeOptions options() {
    vnmaki::ScriptRuntimeOptions result;
    result.scene_state = &scene;
    result.scene = &scene;
    return result;
  }
  [[nodiscard]] const DisplayProperties& props(const std::string& tag) const {
    const vnmaki::Displayable* shown = scene.find(tag);
    REQUIRE(shown != nullptr);
    return shown->properties();
  }
  [[nodiscard]] bool at_wait() const {
    const vnmaki::BlockingRequest* request = runtime.request();
    return request != nullptr && std::holds_alternative<vnmaki::WaitRequest>(*request);
  }

  vnmaki::Vfs vfs;
  Scene scene;
  vnmaki::ScriptRuntime runtime;
};

// The error of a story with a shown `eileen` whose `start` label then runs `lines` (from line 3).
std::string line_error(const std::string& lines) {
  const Stage stage{"label(\"start\", function()\n  show(\"eileen\", { at = \"left\" })\n" + lines +
                    "\nend)\n"};
  REQUIRE(stage.runtime.state() == vnmaki::RunState::kFailed);
  return stage.runtime.error();
}

}  // namespace

// --- Easings ---------------------------------------------------------------------------------

TEST_CASE("anim: every easing is 0 at 0, 1 at 1 and the known value at one half") {
  for (const EaseCase& tested : kEaseCases) {
    CAPTURE(vnmaki::ease_name(tested.ease));
    CHECK(vnmaki::apply_ease(tested.ease, 0) == doctest::Approx(0).epsilon(kEpsilon));
    CHECK(vnmaki::apply_ease(tested.ease, kHalf) == doctest::Approx(tested.half).epsilon(kEpsilon));
    CHECK(vnmaki::apply_ease(tested.ease, 1) == doctest::Approx(1).epsilon(kEpsilon));
  }
}

TEST_CASE("anim: progress outside [0, 1] is clamped, and easings have script names") {
  CHECK(vnmaki::apply_ease(Ease::kOutQuad, -1) == doctest::Approx(0));
  CHECK(vnmaki::apply_ease(Ease::kOutQuad, 2) == doctest::Approx(1));
  CHECK(vnmaki::apply_ease(Ease::kOutQuad, std::nan("")) == doctest::Approx(0));
  for (const EaseCase& tested : kEaseCases) {
    CHECK(vnmaki::ease_from_name(vnmaki::ease_name(tested.ease)) == tested.ease);
  }
  CHECK(vnmaki::ease_name(Ease::kInOutBounce) == "in_out_bounce");
  CHECK_FALSE(vnmaki::ease_from_name("bounce").has_value());
  CHECK_FALSE(vnmaki::ease_from_name("").has_value());
}

// --- The timeline ----------------------------------------------------------------------------

TEST_CASE("timeline: a tween moves a property from where it is to its target") {
  Bench bench;
  bench.shown["a"].position.x = kMiddle;
  REQUIRE(bench.start(move("a", Channel::kX, kFar, 2)).empty());
  CHECK(bench.timeline.busy("a"));
  CHECK(bench.timeline.busy(""));
  CHECK_FALSE(bench.timeline.busy("b"));

  bench.run(1, 1);
  CHECK(bench.x() == doctest::Approx(kThreeQuarters * kFar).epsilon(kEpsilon));  // 50 + 50 / 2
  CHECK(bench.timeline.busy("a"));
  bench.run(1, 1);
  CHECK(bench.x() == kFar);
  CHECK_FALSE(bench.timeline.busy("a"));
  CHECK(bench.timeline.empty());
  bench.run(1, 1);
  CHECK(bench.x() == kFar);
}

TEST_CASE("timeline: a delay holds the tween back, and it reads the start when it begins") {
  Bench bench;
  TweenNode node = move("a", Channel::kX, kFar, 1);
  node.delay = 1;
  REQUIRE(bench.start(node).empty());
  bench.shown["a"].position.x = kMiddle;  // changed during the delay
  bench.run(kHalf, kHalf);
  CHECK(bench.x() == kMiddle);
  bench.run(1, kHalf);  // 0.5 s into the tween
  CHECK(bench.x() == doctest::Approx(kThreeQuarters * kFar).epsilon(kEpsilon));
  bench.run(kHalf, kHalf);
  CHECK(bench.x() == kFar);
  CHECK(bench.timeline.empty());
}

TEST_CASE("timeline: a sequence runs its tweens one after another") {
  Bench bench;
  REQUIRE(bench
              .start(group(Kind::kSequence,
                           {move("a", Channel::kX, kFar, 1), move("a", Channel::kX, kFarther, 1)}))
              .empty());
  bench.run(kHalf, kHalf);
  CHECK(bench.x() == doctest::Approx(kMiddle).epsilon(kEpsilon));
  bench.run(1, kHalf);  // 1.5 s: halfway from 100 to 300
  CHECK(bench.x() == doctest::Approx(kTwoHundred).epsilon(kEpsilon));
  CHECK(bench.timeline.busy("a"));
  bench.run(kHalf, kHalf);
  CHECK(bench.x() == kFarther);
  CHECK_FALSE(bench.timeline.busy("a"));
}

TEST_CASE("timeline: a parallel group runs its tweens together and ends with the last") {
  Bench bench;
  REQUIRE(bench
              .start(group(Kind::kParallel,
                           {move("a", Channel::kX, kFar, 1), move("b", Channel::kY, kFar, 2)}))
              .empty());
  bench.run(1, 1);
  CHECK(bench.x("a") == kFar);
  CHECK(bench.y("b") == doctest::Approx(kMiddle).epsilon(kEpsilon));
  CHECK(bench.timeline.busy("b"));
  bench.run(1, 1);
  CHECK(bench.y("b") == kFar);
  CHECK(bench.timeline.empty());
}

TEST_CASE("timeline: a group delay delays all its children, and groups nest") {
  Bench bench;
  TweenNode inner =
      group(Kind::kParallel, {move("a", Channel::kX, kFar, 1), move("b", Channel::kX, kFar, 1)});
  inner.delay = 1;
  REQUIRE(bench.start(group(Kind::kSequence, {move("a", Channel::kY, kFar, 1), inner})).empty());
  bench.run(1, 1);
  CHECK(bench.y("a") == kFar);
  CHECK(bench.x("a") == 0);
  bench.run(kHalf + 1, kHalf);  // 0.5 s into the inner group, after its delay
  CHECK(bench.x("a") == doctest::Approx(kMiddle).epsilon(kEpsilon));
  CHECK(bench.x("b") == doctest::Approx(kMiddle).epsilon(kEpsilon));
}

TEST_CASE("timeline: a looping tween restarts and never blocks a wait") {
  Bench bench;
  TweenNode node = move("a", Channel::kX, kFar, 1);
  node.loop = true;
  REQUIRE(bench.start(node).empty());
  CHECK_FALSE(bench.timeline.busy("a"));
  bench.run(kHalf, kHalf);
  CHECK(bench.x() == doctest::Approx(kMiddle).epsilon(kEpsilon));
  bench.run(kThreeQuarters, kQuarter);  // 1.25 s: a quarter into the second turn
  CHECK(bench.x() == doctest::Approx(kQuarter * kFar).epsilon(kEpsilon));
  CHECK_FALSE(bench.timeline.empty());
  bench.timeline.finish("", bench.lookup());  // finish leaves loops running
  CHECK_FALSE(bench.timeline.empty());
}

TEST_CASE("timeline: yoyo goes to the target and back") {
  Bench bench;
  TweenNode once = move("a", Channel::kX, kFar, 1);
  once.yoyo = true;
  REQUIRE(bench.start(once).empty());
  bench.run(kHalf, kHalf);
  CHECK(bench.x() == doctest::Approx(kMiddle).epsilon(kEpsilon));
  bench.run(kHalf, kHalf);
  CHECK(bench.x() == doctest::Approx(kFar).epsilon(kEpsilon));
  bench.run(kHalf, kHalf);
  CHECK(bench.x() == doctest::Approx(kMiddle).epsilon(kEpsilon));
  CHECK(bench.timeline.busy("a"));
  bench.run(kHalf, kHalf);
  CHECK(bench.x() == 0);
  CHECK(bench.timeline.empty());

  TweenNode forever = move("b", Channel::kX, kFar, 1);
  forever.yoyo = true;
  forever.loop = true;
  REQUIRE(bench.start(forever).empty());
  bench.run(2 + kHalf, kHalf);  // 2.5 s: half way up in the second turn
  CHECK(bench.x("b") == doctest::Approx(kMiddle).epsilon(kEpsilon));
  bench.run(1, kHalf);  // 3.5 s: half way down
  CHECK(bench.x("b") == doctest::Approx(kMiddle).epsilon(kEpsilon));
  bench.run(kHalf, kHalf);  // 4 s: back at the start
  CHECK(bench.x("b") == doctest::Approx(0).epsilon(kEpsilon));
}

TEST_CASE("timeline: a looping group repeats as a whole") {
  Bench bench;
  TweenNode loop = group(Kind::kSequence,
                         {move("a", Channel::kX, kFar, 1), move("a", Channel::kX, kFarther, 1)});
  loop.loop = true;
  REQUIRE(bench.start(loop).empty());
  bench.run(2 + kHalf, kHalf);  // the second turn, 0.5 s in: the first tween again, from 0
  CHECK(bench.x() == doctest::Approx(kMiddle).epsilon(kEpsilon));
  bench.run(1, kHalf);  // 1.5 s into the turn
  CHECK(bench.x() == doctest::Approx(kTwoHundred).epsilon(kEpsilon));
}

TEST_CASE("timeline: steps of 1/60 s and 1/7 s give the same values") {
  constexpr double kSixtieth = 1.0 / 60;
  constexpr double kSeventh = 1.0 / 7;
  constexpr double kFirst = 0.8;
  constexpr double kSecond = 0.6;
  constexpr double kFade = 0.5;
  constexpr double kFadeDelay = 0.1;
  constexpr double kLast = 0.8;
  const auto build = [] {
    TweenNode first = move("a", Channel::kX, kFar, kFirst);
    first.ease = Ease::kOutBounce;
    TweenNode second = move("a", Channel::kX, kMiddle, kSecond);
    second.ease = Ease::kInOutBack;
    TweenNode fade = move("b", Channel::kAlpha, 0, kFade);
    fade.delay = kFadeDelay;
    fade.ease = Ease::kInSine;
    TweenNode last = move("b", Channel::kY, kFarther, kLast);
    last.ease = Ease::kOutCubic;
    return group(Kind::kSequence, {first, group(Kind::kParallel, {second, fade}), last});
  };
  // A whole number of both step sizes: 60 and 7 steps make one second. At 1 s the parallel group
  // is under way, at 2 s the last tween is.
  for (const double moment : {1.0, 2.0}) {
    CAPTURE(moment);
    Bench fine;
    Bench coarse;
    REQUIRE(fine.start(build()).empty());
    REQUIRE(coarse.start(build()).empty());
    fine.run(moment, kSixtieth);
    coarse.run(moment, kSeventh);
    CHECK(coarse.x() == doctest::Approx(fine.x()).epsilon(kEpsilon));
    CHECK(coarse.shown.at("b").alpha ==
          doctest::Approx(fine.shown.at("b").alpha).epsilon(kEpsilon));
    CHECK(coarse.y("b") == doctest::Approx(fine.y("b")).epsilon(kEpsilon));
    CHECK(fine.timeline.busy("b"));
    if (moment < kThreeQuarters * 2) {
      CHECK(fine.shown.at("b").alpha < 1);  // under way
      CHECK(fine.shown.at("b").alpha > 0);
    } else {
      CHECK(fine.y("b") > 0);
      CHECK(fine.y("b") < kFarther);
    }
  }
  // Both end on exactly the same values.
  Bench fine;
  Bench coarse;
  REQUIRE(fine.start(build()).empty());
  REQUIRE(coarse.start(build()).empty());
  fine.run(3, kSixtieth);
  coarse.run(3, kSeventh);
  CHECK(fine.x() == kMiddle);
  CHECK(coarse.x() == fine.x());
  CHECK(coarse.y("b") == kFarther);
  CHECK(fine.y("b") == kFarther);
  CHECK(coarse.timeline.empty());
  CHECK(fine.timeline.empty());
}

TEST_CASE("timeline: a back easing never leaves the range of opacity and colour") {
  constexpr int kBackSteps = 100;
  constexpr double kBackStep = 0.01;
  Bench bench;
  TweenNode fade = move("a", Channel::kAlpha, 0, 1);
  fade.ease = Ease::kInOutBack;
  fade.to.at(vnmaki::slot(Channel::kTintG)) = 0.0F;
  REQUIRE(bench.start(fade).empty());
  for (int i = 0; i < kBackSteps; ++i) {
    bench.run(kBackStep, kBackStep);
    CHECK(bench.shown.at("a").alpha >= 0);
    CHECK(bench.shown.at("a").alpha <= 1);
    CHECK(bench.shown.at("a").tint.g >= 0);
  }
}

TEST_CASE("timeline: a replay puts tweens at their end values and starts loops at 0") {
  Bench bench;
  REQUIRE(bench
              .start(group(Kind::kSequence,
                           {move("a", Channel::kX, kFar, 1), move("a", Channel::kY, kFar, 1)}),
                     true)
              .empty());
  CHECK(bench.x() == kFar);
  CHECK(bench.y() == kFar);
  CHECK(bench.timeline.empty());

  TweenNode loop = move("b", Channel::kX, kFar, 1);
  loop.loop = true;
  REQUIRE(bench.start(loop, true).empty());
  CHECK(bench.x("b") == 0);
  CHECK_FALSE(bench.timeline.empty());
}

TEST_CASE("timeline: finish puts the tweens of a tag at their end values") {
  Bench bench;
  REQUIRE(bench.start(move("a", Channel::kX, kFar, 1)).empty());
  REQUIRE(bench.start(move("b", Channel::kX, kFar, 1)).empty());
  bench.timeline.finish("a", bench.lookup());
  CHECK(bench.x("a") == kFar);
  CHECK(bench.x("b") == 0);
  CHECK_FALSE(bench.timeline.busy("a"));
  CHECK(bench.timeline.busy("b"));
  bench.timeline.finish("", bench.lookup());
  CHECK(bench.x("b") == kFar);
  CHECK(bench.timeline.empty());
}

TEST_CASE("timeline: prune drops tweens of displayables that are gone") {
  Bench bench;
  REQUIRE(bench
              .start(group(Kind::kParallel,
                           {move("a", Channel::kX, kFar, 1), move("b", Channel::kX, kFar, 1)}))
              .empty());
  bench.shown.erase("a");
  bench.timeline.prune(bench.lookup());
  CHECK_FALSE(bench.timeline.busy("a"));
  CHECK(bench.timeline.busy("b"));
  bench.shown.erase("b");
  bench.timeline.prune(bench.lookup());
  CHECK(bench.timeline.empty());
}

TEST_CASE("timeline: start explains what it cannot run, and starts nothing") {
  Bench bench;
  CHECK(bench.start(move("ghost", Channel::kX, kFar, 1)) == "no displayable 'ghost' is shown");
  TweenNode nested_loop = group(Kind::kParallel, {move("a", Channel::kX, kFar, 1)});
  nested_loop.loop = true;
  CHECK(bench.start(group(Kind::kSequence, {nested_loop})) ==
        "only a tween or the outermost group can loop");
  TweenNode loop = move("a", Channel::kX, kFar, 1);
  loop.loop = true;
  CHECK(bench.start(group(Kind::kSequence, {loop, move("a", Channel::kY, kFar, 1)})) ==
        "a looping tween must be the last item of a sequence");
  CHECK(bench.timeline.empty());
  CHECK(bench.start(group(Kind::kSequence, {move("a", Channel::kY, kFar, 1), loop})).empty());
  CHECK_FALSE(bench.timeline.empty());
}

TEST_CASE("timeline: the snapshot keeps loops, which restart at time 0 from where they began") {
  Bench bench;
  bench.shown["a"].position.x = kMiddle;
  TweenNode loop = move("a", Channel::kX, kFar, 1);
  loop.loop = true;
  loop.yoyo = true;
  loop.ease = Ease::kInOutQuad;
  REQUIRE(bench.start(loop).empty());
  REQUIRE(bench.start(move("b", Channel::kY, kFar, 2)).empty());
  bench.run(kHalf + kQuarter, kQuarter);  // the loop is mid-way

  const Bytes bytes = bench.timeline.capture();
  Timeline settled = bench.timeline;
  auto resting = bench.shown;
  settled.settle([&resting](std::string_view tag) -> DisplayProperties* {
    const auto found = resting.find(tag);
    return found == resting.end() ? nullptr : &found->second;
  });
  CHECK(resting.at("a").position.x == kMiddle);  // where the loop began
  CHECK(resting.at("b").position.y == kFar);     // the tween that ends is at its end value

  Bench restored;
  restored.shown = resting;
  restored.timeline.restore(bytes, restored.lookup());
  CHECK(restored.timeline.capture() == bytes);
  CHECK_FALSE(restored.timeline.busy(""));
  restored.run(kHalf, kHalf);  // time 0 plus half a second: eased progress 0.5
  CHECK(restored.x() == doctest::Approx(kMiddle + (kFar - kMiddle) * kHalf).epsilon(kEpsilon));
}

TEST_CASE("timeline: a malformed snapshot throws and changes nothing") {
  Bench bench;
  TweenNode loop = move("a", Channel::kX, kFar, 1);
  loop.loop = true;
  REQUIRE(bench.start(loop).empty());
  const Bytes bytes = bench.timeline.capture();
  for (std::size_t length = 1; length < bytes.size(); ++length) {
    CHECK_THROWS_AS(bench.timeline.restore(std::span{bytes}.first(length), bench.lookup()),
                    std::invalid_argument);
  }
  Bytes longer = bytes;
  longer.push_back(std::byte{0});
  CHECK_THROWS_AS(bench.timeline.restore(longer, bench.lookup()), std::invalid_argument);
  CHECK(bench.timeline.capture() == bytes);

  // A negative delay.
  vnmaki::ByteWriter writer;
  writer.u32(1);
  writer.f64(-1);
  writer.f64(0);
  writer.u32(0);
  CHECK_THROWS_AS(bench.timeline.restore(writer.take(), bench.lookup()), std::invalid_argument);
  bench.timeline.restore({}, bench.lookup());
  CHECK(bench.timeline.empty());
}

// --- Script: tween, sequence, parallel -------------------------------------------------------

TEST_CASE("script: a tween that is not waited for does not block, and follows the clock") {
  Stage stage{R"(
label("start", function()
  show("eileen", { at = "left" })
  tween("eileen", { x = 0.8, duration = 1 })
  narrate("go")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  CHECK(std::holds_alternative<vnmaki::NarrateRequest>(*stage.runtime.request()));
  CHECK(stage.props("eileen").position.x == kLeftX);
  CHECK(stage.scene.tweening("eileen"));
  stage.scene.update(kHalf);
  CHECK(stage.props("eileen").position.x == doctest::Approx(kMidX).epsilon(kEpsilon));
  stage.scene.update(kHalf);
  CHECK(stage.props("eileen").position.x == kEndX);
  CHECK_FALSE(stage.scene.tweening("eileen"));
  CHECK(stage.props("eileen").position.y == kStageY);
}

TEST_CASE("script: tween changes every property in the units of show") {
  constexpr float kAnchor = 0.25F;
  constexpr float kScale = 2;
  constexpr float kScaleY = 3;
  constexpr float kRotation = 45;
  constexpr float kAlpha = 0.5F;
  constexpr float kCropX = 0.1F;
  constexpr float kCropWidth = 0.5F;
  constexpr float kOrange = 0.5F;  // 0x80 / 255
  Stage stage{R"(
label("start", function()
  show("eileen", { at = "left" })
  tween("eileen", { y = 0.5, anchor = { x = 0.25 }, scale = { x = 2, y = 3 }, rotation = 45,
                    alpha = 0.5, tint = "#ff8000", crop = { x = 0.1, width = 0.5 }, duration = 1 })
  narrate("go")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  stage.scene.update(1);
  const DisplayProperties& shown = stage.props("eileen");
  CHECK(shown.position.x == kLeftX);
  CHECK(shown.position.y == kStageY / 2);
  CHECK(shown.anchor.x == kAnchor);
  CHECK(shown.anchor.y == 1);  // left alone
  CHECK(shown.scale.x == kScale);
  CHECK(shown.scale.y == kScaleY);
  CHECK(shown.rotation == kRotation);
  CHECK(shown.alpha == kAlpha);
  CHECK(shown.tint.r == 1);
  CHECK(shown.tint.g == doctest::Approx(kOrange).epsilon(kEpsilon * 100));
  CHECK(shown.tint.b == 0);
  CHECK(shown.crop.x == kCropX);
  CHECK(shown.crop.width == kCropWidth);
  CHECK(shown.crop.height == 1);
}

TEST_CASE("script: a number as scale sets both axes") {
  constexpr float kScale = 2;
  Stage stage{R"(
label("start", function()
  show("eileen")
  tween("eileen", { scale = 2, duration = 0 })
  narrate("go")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  stage.scene.update(0);
  CHECK(stage.props("eileen").scale.x == kScale);
  CHECK(stage.props("eileen").scale.y == kScale);
}

TEST_CASE("script: sequence and parallel group tweens, and groups nest") {
  constexpr double kOneAndAHalf = 1.5;
  Stage stage{R"(
label("start", function()
  show("eileen", { at = "left" })
  show("lucy", { at = "right" })
  sequence {
    { "eileen", x = 0.5, duration = 1 },
    { parallel = {
        { "eileen", x = 0.75, duration = 1 },
        { "lucy", y = 0, duration = 2 },
      } },
    { "lucy", x = 0.5, duration = 1, delay = 1 },
  }
  wait_all()
  narrate("done")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  REQUIRE(stage.at_wait());
  stage.scene.update(kHalf);
  CHECK(stage.props("eileen").position.x ==
        doctest::Approx((kLeftX + kEyeX) * kHalf).epsilon(kEpsilon));
  stage.scene.update(1);  // 1.5 s: half way through the parallel group
  CHECK(stage.props("eileen").position.x ==
        doctest::Approx((kEyeX + kRightX) * kHalf).epsilon(kEpsilon));
  CHECK(stage.props("lucy").position.y ==
        doctest::Approx(kStageY * kThreeQuarters).epsilon(kEpsilon));
  stage.scene.update(1);  // 2.5 s
  CHECK(stage.props("lucy").position.y == doctest::Approx(kStageY * kQuarter).epsilon(kEpsilon));
  CHECK(stage.props("lucy").position.x == kRightX);
  stage.scene.update(kOneAndAHalf);  // 4 s: the delay of the last tween is over
  CHECK(stage.props("lucy").position.x == kRightX);
  CHECK(stage.scene.tweening(""));
  stage.scene.update(1);  // 5 s
  CHECK(stage.props("lucy").position.x == kEyeX);
  CHECK(stage.props("lucy").position.y == 0);
  CHECK_FALSE(stage.scene.tweening(""));
}

TEST_CASE("script: loop and yoyo are options of a tween and of the outermost group") {
  Stage stage{R"(
label("start", function()
  show("eileen", { at = "left" })
  show("lucy", { at = "right" })
  tween("eileen", { x = 0.5, duration = 1, loop = true, yoyo = true, ease = "in_out_sine" })
  sequence {
    { "lucy", alpha = 0, duration = 1 },
    { "lucy", alpha = 1, duration = 1 },
    loop = true,
  }
  wait_all()
  narrate("done")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  WaitPresenter waits{stage.runtime, stage.scene};
  CHECK_FALSE(stage.scene.tweening(""));
  CHECK(waits.update());  // loops never block a wait
  CHECK(std::holds_alternative<vnmaki::NarrateRequest>(*stage.runtime.request()));
  stage.scene.update(1);
  CHECK(stage.props("eileen").position.x == doctest::Approx(kEyeX).epsilon(kEpsilon));
  stage.scene.update(1);
  CHECK(stage.props("eileen").position.x == doctest::Approx(kLeftX).epsilon(kEpsilon));
  CHECK(stage.props("lucy").alpha == 1);
  stage.scene.update(kHalf);
  CHECK(stage.props("lucy").alpha == doctest::Approx(kHalf).epsilon(kEpsilon));
}

// --- Script: wait ----------------------------------------------------------------------------

constexpr std::string_view kWaitStory = R"(
label("start", function()
  show("eileen", { at = "left" })
  show("lucy", { at = "right" })
  tween("eileen", { x = 0.8, duration = 1 })
  wait("eileen")
  narrate("done")
end)
)";

TEST_CASE("script: wait is a step that ends when its tweens end") {
  Stage stage{std::string{kWaitStory}};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  REQUIRE(stage.at_wait());
  CHECK(std::get<vnmaki::WaitRequest>(*stage.runtime.request()).tag == "eileen");
  CHECK(stage.runtime.position() ==
        vnmaki::ScriptPosition{.label = "start", .step = 1, .wait = true});

  WaitPresenter waits{stage.runtime, stage.scene};
  CHECK(waits.waiting());
  CHECK_FALSE(waits.update());
  stage.scene.update(kHalf);
  CHECK_FALSE(waits.update());
  CHECK(stage.at_wait());
  stage.scene.update(kHalf);
  CHECK(waits.update());
  CHECK_FALSE(waits.waiting());
  CHECK(std::holds_alternative<vnmaki::NarrateRequest>(*stage.runtime.request()));
  CHECK(stage.runtime.position() ==
        vnmaki::ScriptPosition{.label = "start", .step = 2, .wait = false});
  CHECK(stage.props("eileen").position.x == kEndX);
}

TEST_CASE("script: wait is a step also when nothing is tweening, and ignores other tags") {
  Stage stage{R"(
label("start", function()
  show("eileen", { at = "left" })
  tween("eileen", { x = 0.8, duration = 1 })
  wait("lucy")
  wait_all()
  narrate("done")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  WaitPresenter waits{stage.runtime, stage.scene};
  REQUIRE(stage.at_wait());
  CHECK(waits.update());  // lucy is not tweening: the wait is over at once
  REQUIRE(stage.at_wait());
  CHECK(std::get<vnmaki::WaitRequest>(*stage.runtime.request()).tag.empty());
  CHECK_FALSE(waits.update());  // but wait_all() waits for eileen
  stage.scene.update(1);
  CHECK(waits.update());
  CHECK(stage.runtime.position().step == 3);
}

TEST_CASE("script: a click ends a wait at once, other clicks are left to the caller") {
  Stage stage{std::string{kWaitStory}};
  REQUIRE(stage.at_wait());
  WaitPresenter waits{stage.runtime, stage.scene};
  stage.scene.update(kQuarter);
  CHECK(waits.skip());
  CHECK(stage.props("eileen").position.x == kEndX);
  CHECK_FALSE(stage.scene.tweening("eileen"));
  CHECK(std::holds_alternative<vnmaki::NarrateRequest>(*stage.runtime.request()));
  CHECK_FALSE(waits.skip());  // at a line, the click is an advance
}

TEST_CASE("script: advance() also ends a wait") {
  Stage stage{std::string{kWaitStory}};
  REQUIRE(stage.at_wait());
  stage.runtime.advance();
  CHECK(std::holds_alternative<vnmaki::NarrateRequest>(*stage.runtime.request()));
}

TEST_CASE("script: hiding a displayable ends its tweens") {
  Stage stage{R"(
label("start", function()
  show("eileen", { at = "left" })
  tween("eileen", { x = 0.8, duration = 1, loop = true })
  tween("eileen", { y = 0, duration = 1 })
  hide("eileen")
  narrate("go")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  CHECK_FALSE(stage.scene.tweening(""));
  const Stage empty{R"(label("start", function() narrate("go") end))"};
  CHECK(stage.scene.capture() == empty.scene.capture());
}

TEST_CASE("script: scene() ends the tweens of the displayables it clears") {
  Stage stage{R"(
label("start", function()
  show("eileen", { at = "left" })
  tween("eileen", { x = 0.8, duration = 1 })
  scene("eileen")
  narrate("go")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  CHECK_FALSE(stage.scene.tweening(""));
}

TEST_CASE("script: the headless runner ends waits at once and counts them as stops") {
  const vnmaki::Vfs vfs = game_files(std::string{kWaitStory});
  Scene scene{vfs, kVirtualSize};
  vnmaki::HeadlessOptions options;
  options.runtime.scene = &scene;
  options.runtime.scene_state = &scene;
  const std::vector<vnmaki::ScriptInput> inputs{vnmaki::AdvanceInput{}};
  const vnmaki::HeadlessResult result = vnmaki::run_headless(vfs, inputs, options);
  CHECK(result.outcome == vnmaki::HeadlessOutcome::kFinished);
  CHECK(result.transcript == std::vector<std::string>{"label start", "show eileen at left",
                                                      "show lucy at right", "tween eileen",
                                                      "wait eileen", "narrate done"});
  CHECK(result.stops == std::vector<vnmaki::ScriptPosition>{
                            {.label = "start", .step = 1, .wait = true},
                            {.label = "start", .step = 2, .wait = false},
                        });
  CHECK(scene.find("eileen")->properties().position.x == kEndX);
}

// --- Script: replay and snapshot -------------------------------------------------------------

constexpr std::string_view kReplayStory = R"(
label("start", function()
  show("eileen", { at = "left" })
  tween("eileen", { x = 0.8, duration = 1 })
  narrate("one")
  wait("eileen")
  narrate("two")
end)
)";

TEST_CASE("script: a replay puts tweens at their end values, silently") {
  Stage live{std::string{kReplayStory}};
  REQUIRE_MESSAGE(live.runtime.state() == vnmaki::RunState::kWaiting, live.runtime.error());
  live.scene.update(kQuarter);  // mid-tween, at "one"
  CHECK(live.props("eileen").position.x < kEndX);
  REQUIRE(live.runtime.label_snapshot() != nullptr);
  const vnmaki::LabelSnapshot snapshot = *live.runtime.label_snapshot();
  const std::vector<vnmaki::InputResult> inputs{live.runtime.input_log().begin(),
                                                live.runtime.input_log().end()};

  live.runtime.replay(snapshot, inputs, 1);
  REQUIRE_MESSAGE(live.runtime.state() == vnmaki::RunState::kWaiting, live.runtime.error());
  CHECK(live.props("eileen").position.x == kEndX);
  CHECK_FALSE(live.scene.tweening(""));

  live.runtime.advance();  // the wait step
  REQUIRE(live.at_wait());
  CHECK(live.runtime.position().wait);
  // A replay to the wait step itself, in a fresh stage: the same state.
  Stage fresh{std::string{kReplayStory}};
  fresh.runtime.replay(snapshot, inputs, 2);
  REQUIRE_MESSAGE(fresh.runtime.state() == vnmaki::RunState::kWaiting, fresh.runtime.error());
  CHECK(fresh.at_wait());
  CHECK(fresh.props("eileen").position.x == kEndX);
  CHECK(fresh.scene.capture() == live.scene.capture());
}

TEST_CASE("script: loops are in the label-entry snapshot and start again at time 0") {
  constexpr std::string_view kLoopStory = R"(
label("start", function()
  show("eileen", { at = "left" })
  tween("eileen", { y = 0.5, duration = 1, loop = true, yoyo = true })
  narrate("one")
  jump("next")
end)

label("next", function()
  narrate("two")
end)
)";
  Stage live{std::string{kLoopStory}};
  REQUIRE_MESSAGE(live.runtime.state() == vnmaki::RunState::kWaiting, live.runtime.error());
  live.scene.update(kQuarter);  // the loop is under way
  CHECK(live.props("eileen").position.y < kStageY);
  live.runtime.advance();  // into label "next"
  REQUIRE_MESSAGE(live.runtime.state() == vnmaki::RunState::kWaiting, live.runtime.error());
  REQUIRE(live.runtime.position().label == "next");
  REQUIRE(live.runtime.label_snapshot() != nullptr);
  const vnmaki::LabelSnapshot snapshot = *live.runtime.label_snapshot();

  Stage fresh{std::string{kLoopStory}};
  fresh.runtime.replay(snapshot, {}, 1);
  REQUIRE_MESSAGE(fresh.runtime.state() == vnmaki::RunState::kWaiting, fresh.runtime.error());
  CHECK(fresh.props("eileen").position.y == kStageY);  // where the loop began
  fresh.scene.update(kHalf);                           // the loop runs again from time 0
  CHECK(fresh.props("eileen").position.y ==
        doctest::Approx(kStageY * kThreeQuarters).epsilon(kEpsilon));
  CHECK_FALSE(fresh.scene.tweening(""));
}

TEST_CASE("scene: a captured state with a loop restores to the same bytes") {
  Stage stage{R"(
label("start", function()
  show("eileen", { at = "left" })
  tween("eileen", { x = 0.8, duration = 1, loop = true })
  tween("eileen", { y = 0, duration = 1 })
  narrate("one")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  stage.scene.update(kQuarter);
  const Bytes bytes = stage.scene.capture();
  // The snapshot shows what a replay would leave: the loop at its start, the tween at its end.
  Stage other{R"(label("start", function() narrate("one") end))"};
  other.scene.restore(bytes);
  CHECK(other.props("eileen").position.x == kLeftX);
  CHECK(other.props("eileen").position.y == 0);
  CHECK(other.scene.capture() == bytes);
  // Capturing does not disturb the live scene.
  CHECK(stage.props("eileen").position.y > 0);
  CHECK(stage.scene.tweening("eileen"));
}

TEST_CASE("scene: the snapshot is a map of parts, and parts it does not know are left out") {
  const vnmaki::Vfs vfs = game_files("");
  Scene scene{vfs, kVirtualSize};
  const auto state_with = [](std::uint32_t version, const std::vector<std::string>& part_names) {
    vnmaki::ByteWriter writer;
    writer.u32(version);
    writer.u32(static_cast<std::uint32_t>(part_names.size()));
    for (const std::string& name : part_names) {
      writer.string(name);
      writer.block({});
    }
    return writer.take();
  };
  CHECK_NOTHROW(scene.restore(state_with(2, {"future part"})));
  CHECK(scene.tags(vnmaki::Layer::kCharacters).empty());
  CHECK_THROWS_AS(scene.restore(state_with(2, {"twice", "twice"})), std::invalid_argument);
  CHECK_THROWS_AS(scene.restore(state_with(1, {})), std::invalid_argument);
  // A part that is there but empty is malformed.
  CHECK_THROWS_AS(scene.restore(state_with(2, {"displayables"})), std::invalid_argument);
}

// --- Script: errors --------------------------------------------------------------------------

TEST_CASE("script: bad tween and wait calls name the script line") {
  CHECK(line_error(R"(tween("eileen", { ease = "wobble" }))") ==
        "scripts/main.lua:3: unknown easing 'wobble' (use linear, or in_, out_ or in_out_ followed "
        "by sine, quad, cubic, back or bounce)");
  CHECK(line_error(R"(tween("ghost", { x = 1 }))") ==
        "scripts/main.lua:3: no displayable 'ghost' is shown");
  CHECK(line_error(R"(tween("eileen", { x = "far" }))") ==
        "scripts/main.lua:3: bad option 'x' to 'tween' (number expected, got string)");
  CHECK(line_error(R"(tween("eileen", { loop = 1 }))") ==
        "scripts/main.lua:3: bad option 'loop' to 'tween' (boolean expected, got number)");
  CHECK(line_error(R"(tween("eileen", { tint = "red" }))") ==
        R"x(scripts/main.lua:3: bad option 'tint' to 'tween' ("#rrggbb" expected))x");
  CHECK(line_error(R"(tween("eileen", { anchor = { x = "a" } }))") ==
        "scripts/main.lua:3: bad field 'x' of option 'anchor' to 'tween' (number expected, got "
        "string)");
  CHECK(line_error(R"(tween("eileen", { crop = 1 }))") ==
        "scripts/main.lua:3: bad option 'crop' to 'tween' (table expected, got number)");
  CHECK(line_error(R"(tween("eileen", { delay = -1 }))") ==
        "scripts/main.lua:3: option 'delay' must be a number from 0 to 86400");
  CHECK(line_error(R"(tween("eileen", { duration = 0 / 0 }))") ==
        "scripts/main.lua:3: option 'duration' must be a number from 0 to 86400");
  CHECK(line_error(R"(tween("eileen", { x = 1e300 }))") ==
        "scripts/main.lua:3: option 'x' must be a number from -1000000 to 1000000");
  CHECK(line_error("tween(42)") ==
        "scripts/main.lua:3: bad argument #1 to 'tween' (string expected, got number)");
  CHECK(line_error(R"(tween("eileen", "x"))") ==
        "scripts/main.lua:3: bad argument #2 to 'tween' (table or nil expected, got string)");
  CHECK(line_error("sequence {}") == "scripts/main.lua:3: sequence() needs at least one item");
  CHECK(line_error("parallel(1)") ==
        "scripts/main.lua:3: bad argument #1 to 'parallel' (table expected, got number)");
  CHECK(line_error("sequence { 1 }") ==
        "scripts/main.lua:3: bad item #1 in 'sequence' (a table expected, got number)");
  CHECK(line_error(R"(sequence { { "eileen", x = "far" } })") ==
        "scripts/main.lua:3: bad option 'x' to 'sequence' (number expected, got string)");
  CHECK(line_error(R"(sequence { { loop = true } })") ==
        "scripts/main.lua:3: bad tween tag (a tag string expected, got nil)");
  CHECK(line_error(R"(sequence { { sequence = 1 } })") ==
        "scripts/main.lua:3: bad item #1 in 'sequence' ('sequence' must be a table of items)");
  CHECK(line_error(R"(sequence { { parallel = { { "eileen", x = 1 } }, loop = true } })") ==
        "scripts/main.lua:3: only a tween or the outermost group can loop");
  CHECK(line_error(R"(sequence { { "eileen", x = 1, loop = true }, { "eileen", y = 0 } })") ==
        "scripts/main.lua:3: a looping tween must be the last item of a sequence");
  CHECK(line_error(R"(wait(""))") ==
        "scripts/main.lua:3: wait() needs a tag (wait_all() waits for every tween)");
  CHECK(line_error("wait(1)") ==
        "scripts/main.lua:3: bad argument #1 to 'wait' (string expected, got number)");
}

TEST_CASE("script: groups nest only so deep") {
  constexpr int kDeep = 20;
  std::string item = R"({ "eileen", x = 1 })";
  for (int i = 0; i < kDeep; ++i) {
    std::string wrapped = "{ sequence = { ";
    wrapped += item;
    wrapped += " } }";
    item = std::move(wrapped);
  }
  CHECK(line_error("sequence { " + item + " }") ==
        "scripts/main.lua:3: groups nest more than 16 levels deep");
}

TEST_CASE("script: tween calls without a scene only reach the observer") {
  const vnmaki::Vfs vfs = game_files(R"(
label("start", function()
  tween("eileen", { x = 1 })
  sequence { { "eileen", x = 1 } }
  wait("eileen")
  narrate("done")
end)
)");
  const std::vector<vnmaki::ScriptInput> inputs{vnmaki::AdvanceInput{}};
  const vnmaki::HeadlessResult result = vnmaki::run_headless(vfs, inputs);
  CHECK(result.outcome == vnmaki::HeadlessOutcome::kFinished);
  CHECK(result.transcript == std::vector<std::string>{"label start", "tween eileen", "sequence",
                                                      "wait eileen", "narrate done"});
}
