// Transitions (docs/SPEC.md section 10): the built-in names and options, the clock, and the
// script's `with` and `transition` calls: they block like `wait`, a click ends them, a replay
// does not show them and a rollback goes back over them. Time is a fake clock. The frames are
// checked on a GPU in tests/render/transition_render_test.cpp.

#include "engine/anim/transition.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/anim/wait_presenter.hpp"
#include "engine/render/shader_compiler.hpp"
#include "engine/runtime/headless_runner.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::Scene;
using vnmaki::Transition;
using vnmaki::TransitionKind;
using vnmaki::TransitionRequest;
using vnmaki::TransitionSpec;
using vnmaki::WaitPresenter;

constexpr double kQuarter = 0.25;
constexpr double kHalf = 0.5;
constexpr double kSecond = 1.0;
constexpr double kSeventh = 1.0 / 7.0;
constexpr double kSixtieth = 1.0 / 60.0;
constexpr double kEpsilon = 1e-9;
constexpr float kSoft = 0.25F;
constexpr std::uint32_t kOrange = 0xFF8000;
constexpr std::uint8_t kOrangeRed = 0xFF;
constexpr std::uint8_t kOrangeGreen = 0x80;
constexpr vnmaki::Size kVirtualSize{.width = 1280, .height = 720};

// A request that is checked, and why not.
std::string check(const TransitionRequest& request, TransitionSpec& spec) {
  return vnmaki::make_transition_spec(request, spec);
}
std::string check(const TransitionRequest& request) {
  TransitionSpec spec;
  return check(request, spec);
}

// --- The built-in names and options ----------------------------------------------------------

TEST_CASE("transition: the built-in names give a kind and a direction") {
  TransitionSpec spec;
  REQUIRE(check({.name = "dissolve"}, spec).empty());
  CHECK(spec.kind == TransitionKind::kDissolve);
  CHECK(spec.duration == vnmaki::kDefaultTransitionSeconds);

  REQUIRE(check({.name = "fade"}, spec).empty());
  CHECK(spec.kind == TransitionKind::kFade);

  REQUIRE(check({.name = "slide_left"}, spec).empty());
  CHECK(spec.kind == TransitionKind::kSlide);
  CHECK(spec.direction == vnmaki::PointF{.x = -1, .y = 0});
  REQUIRE(check({.name = "slide_down"}, spec).empty());
  CHECK(spec.direction == vnmaki::PointF{.x = 0, .y = 1});
  REQUIRE(check({.name = "wipe_up"}, spec).empty());
  CHECK(spec.kind == TransitionKind::kWipe);
  CHECK(spec.direction == vnmaki::PointF{.x = 0, .y = -1});
  REQUIRE(check({.name = "wipe_right"}, spec).empty());
  CHECK(spec.direction == vnmaki::PointF{.x = 1, .y = 0});

  REQUIRE(check({.name = "mask", .mask = "masks/x", .softness = kSoft}, spec).empty());
  CHECK(spec.kind == TransitionKind::kMask);
  CHECK(spec.mask == "masks/x");
  CHECK(spec.softness == kSoft);

  // Any other name is a game shader that the scene looks for.
  REQUIRE(check({.name = "swirl"}, spec).empty());
  CHECK(spec.kind == TransitionKind::kCustom);
}

TEST_CASE("transition: options are checked for the transition they belong to") {
  TransitionSpec spec;
  REQUIRE(check({.name = "fade", .duration = kSecond, .color = kOrange}, spec).empty());
  CHECK(spec.duration == kSecond);
  CHECK(spec.color == vnmaki::rgba8(kOrangeRed, kOrangeGreen, 0));

  CHECK(check({.name = "dissolve", .duration = -1}) ==
        "option 'duration' must be a number from 0 to 86400");
  CHECK(check({.name = "dissolve", .duration = std::nan("")}) ==
        "option 'duration' must be a number from 0 to 86400");
  CHECK(check({.name = "wipe_left", .softness = 2}) ==
        "option 'softness' must be a number from 0 to 1");
  CHECK(check({.name = "dissolve", .softness = kHalf}) ==
        "option 'softness' does not apply to the transition 'dissolve'");
  CHECK(check({.name = "wipe_left", .color = kOrange}) ==
        "option 'color' does not apply to the transition 'wipe_left'");
  CHECK(check({.name = "dissolve", .mask = "masks/x"}) ==
        "option 'mask' does not apply to the transition 'dissolve'");
  CHECK(check({.name = "mask"}) == "the transition 'mask' needs the option 'mask' (an image name)");
  CHECK(check({.name = "dissolve", .uniforms = {{"amount", 1}}}) ==
        "option 'uniforms' does not apply to the transition 'dissolve'");
  CHECK(check({.name = "swirl", .uniforms = {{"amount", 1}}}).empty());
}

// --- The clock -------------------------------------------------------------------------------

TEST_CASE("transition: progress is linear and does not depend on the step size") {
  TransitionSpec spec;
  spec.duration = 2;
  for (const double step : {kSixtieth, kSeventh, kHalf}) {
    CAPTURE(step);
    Transition transition{spec};
    CHECK(transition.progress() == 0);
    CHECK_FALSE(transition.finished());
    double elapsed = 0;
    while (elapsed + step <= kSecond + kEpsilon) {
      transition.advance(step);
      elapsed += step;
    }
    CHECK(transition.progress() == doctest::Approx(kHalf).epsilon(kEpsilon));
    CHECK_FALSE(transition.finished());
    while (elapsed + step <= spec.duration + kEpsilon) {
      transition.advance(step);
      elapsed += step;
    }
    CHECK(transition.finished());
    CHECK(transition.progress() == doctest::Approx(1).epsilon(kEpsilon));
  }
}

TEST_CASE("transition: progress stops at 1, and a duration of 0 is over at once") {
  TransitionSpec spec;
  spec.duration = 1;
  Transition transition{spec};
  transition.advance(kQuarter);
  CHECK(transition.progress() == doctest::Approx(kQuarter));
  transition.advance(kSecond);
  CHECK(transition.progress() == 1);
  CHECK(transition.finished());

  spec.duration = 0;
  const Transition instant{spec};
  CHECK(instant.finished());
  CHECK(instant.progress() == 1);
}

TEST_CASE(
    "transition: the constant buffer has the progress, the soft edge, the way and the colour") {
  TransitionSpec spec;
  REQUIRE(check({.name = "wipe_left", .duration = 2, .softness = kQuarter}, spec).empty());
  Transition transition{spec};
  transition.advance(kHalf);
  const vnmaki::TransitionUniforms uniforms = transition.uniforms();
  CHECK(uniforms.progress == doctest::Approx(kQuarter));
  CHECK(uniforms.softness == doctest::Approx(kQuarter));
  CHECK(uniforms.direction_x == -1);
  CHECK(uniforms.direction_y == 0);
  CHECK(uniforms.color.at(3) == 1);
}

// --- The script ------------------------------------------------------------------------------

vnmaki::Vfs game_files(const std::string& script) {
  return vnmaki::test::memory_vfs({
      {"scripts/main.lua", script},
      {"images/eileen.png", ""},
      {"images/lucy.png", ""},
      {"images/bg/room.png", ""},
      {"images/masks/x.png", ""},
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
  [[nodiscard]] bool at_transition() const {
    const vnmaki::BlockingRequest* request = runtime.request();
    const auto* wait = request != nullptr ? std::get_if<vnmaki::WaitRequest>(request) : nullptr;
    return wait != nullptr && wait->tag == vnmaki::kTransitionTag;
  }
  [[nodiscard]] std::string text() const {
    const auto* narrate = std::get_if<vnmaki::NarrateRequest>(runtime.request());
    return narrate != nullptr ? narrate->text : std::string{};
  }

  vnmaki::Vfs vfs;
  Scene scene;
  vnmaki::ScriptRuntime runtime;
};

// The error of the story whose `start` label runs `lines` (from line 3).
std::string line_error(const std::string& lines) {
  const Stage stage{"label(\"start\", function()\n  show(\"eileen\")\n" + lines + "\nend)\n"};
  REQUIRE(stage.runtime.state() == vnmaki::RunState::kFailed);
  return stage.runtime.error();
}

constexpr std::string_view kStory = R"(
label("start", function()
  narrate("one")
  show("eileen", { with = "dissolve" })
  narrate("two")
end)
)";

TEST_CASE("script: with blocks like wait until the transition ends") {
  Stage stage{std::string{kStory}};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  stage.runtime.advance();
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  REQUIRE(stage.at_transition());
  CHECK(stage.runtime.position() ==
        vnmaki::ScriptPosition{.label = "start", .step = 2, .wait = true});
  CHECK(stage.scene.tweening(vnmaki::kTransitionTag));
  CHECK(stage.scene.find("eileen") != nullptr);  // the change itself is made at once

  WaitPresenter waits{stage.runtime, stage.scene};
  CHECK(waits.waiting());
  CHECK_FALSE(waits.update());
  stage.scene.update(kQuarter);
  CHECK_FALSE(waits.update());
  stage.scene.update(kQuarter);  // the default duration is half a second
  CHECK_FALSE(stage.scene.tweening(vnmaki::kTransitionTag));
  CHECK(waits.update());
  CHECK(stage.text() == "two");
  CHECK(stage.runtime.position() ==
        vnmaki::ScriptPosition{.label = "start", .step = 3, .wait = false});
}

TEST_CASE("script: a click ends a transition at once") {
  Stage stage{std::string{kStory}};
  stage.runtime.advance();
  REQUIRE(stage.at_transition());
  WaitPresenter waits{stage.runtime, stage.scene};
  stage.scene.update(kQuarter);
  CHECK(waits.skip());
  CHECK_FALSE(stage.scene.tweening(vnmaki::kTransitionTag));
  CHECK(stage.text() == "two");
  CHECK_FALSE(waits.skip());  // at a line, the click is an advance
}

TEST_CASE("script: advance() ends a transition too") {
  Stage stage{std::string{kStory}};
  stage.runtime.advance();
  REQUIRE(stage.at_transition());
  stage.runtime.advance();
  CHECK(stage.text() == "two");
}

TEST_CASE("script: all the changes of a step use one transition") {
  Stage stage{R"(
label("start", function()
  scene("bg/room")
  show("eileen", { at = "left" })
  show("lucy", { at = "right", with = { "wipe_left", duration = 2 } })
  narrate("done")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  REQUIRE(stage.at_transition());
  CHECK(stage.runtime.position().step == 1);
  CHECK(stage.scene.find("eileen") != nullptr);
  stage.scene.update(kSecond);
  CHECK(stage.scene.tweening(vnmaki::kTransitionTag));  // two seconds
  stage.scene.update(kSecond);
  CHECK_FALSE(stage.scene.tweening(vnmaki::kTransitionTag));
  stage.runtime.advance();
  CHECK(stage.text() == "done");
  CHECK(stage.runtime.position().step == 2);
}

TEST_CASE("script: transition() mixes the changes made before it") {
  Stage stage{R"(
label("start", function()
  scene("bg/room")
  show("eileen")
  transition("fade", { duration = 1, color = "#ff8000" })
  hide("eileen", { with = "slide_up" })
  scene("bg/room", { with = { "mask", mask = "masks/x", softness = 0.25 } })
  narrate("done")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  const auto step_through = [&] {
    REQUIRE(stage.at_transition());
    CHECK(stage.scene.tweening(vnmaki::kTransitionTag));
    stage.scene.update(kSecond);
    WaitPresenter waits{stage.runtime, stage.scene};
    CHECK(waits.update());
  };
  CHECK(stage.scene.find("eileen") != nullptr);
  step_through();  // the fade ends, and hide slides
  CHECK(stage.scene.find("eileen") == nullptr);
  step_through();  // the slide ends, and scene masks
  step_through();  // the mask ends
  CHECK(stage.text() == "done");
}

TEST_CASE("script: a transition with no time is a cut that is still a step") {
  Stage stage{R"(
label("start", function()
  show("eileen", { with = { "dissolve", duration = 0 } })
  narrate("done")
end)
)"};
  REQUIRE(stage.at_transition());
  CHECK_FALSE(stage.scene.tweening(vnmaki::kTransitionTag));
  WaitPresenter waits{stage.runtime, stage.scene};
  CHECK(waits.update());
  CHECK(stage.text() == "done");
}

TEST_CASE("script: a replay counts the transition as a step and does not show it") {
  Stage live{std::string{kStory}};
  live.runtime.advance();
  REQUIRE(live.at_transition());
  live.runtime.advance();
  REQUIRE(live.text() == "two");
  REQUIRE(live.runtime.label_snapshot() != nullptr);
  const vnmaki::LabelSnapshot snapshot = *live.runtime.label_snapshot();
  const std::vector<vnmaki::InputResult> inputs{live.runtime.input_log().begin(),
                                                live.runtime.input_log().end()};

  Stage fresh{std::string{kStory}};
  fresh.runtime.replay(snapshot, inputs, 3);  // the line after the transition
  REQUIRE_MESSAGE(fresh.runtime.state() == vnmaki::RunState::kWaiting, fresh.runtime.error());
  CHECK(fresh.text() == "two");
  CHECK(fresh.runtime.position().step == 3);
  CHECK_FALSE(fresh.scene.tweening(vnmaki::kTransitionTag));
  CHECK(fresh.scene.find("eileen") != nullptr);

  // A replay to the transition's own step: nothing runs, so the wait is over at once.
  Stage at_step{std::string{kStory}};
  at_step.runtime.replay(snapshot, inputs, 2);
  REQUIRE(at_step.at_transition());
  CHECK_FALSE(at_step.scene.tweening(vnmaki::kTransitionTag));
  WaitPresenter waits{at_step.runtime, at_step.scene};
  CHECK(waits.update());
}

TEST_CASE("script: rollback goes back over a transition like over a wait") {
  Stage stage{std::string{kStory}};
  REQUIRE(stage.text() == "one");
  stage.runtime.advance();
  REQUIRE(stage.at_transition());
  SUBCASE("from the transition") {
    CHECK(stage.runtime.roll_back());
    CHECK(stage.text() == "one");
    CHECK(stage.scene.find("eileen") == nullptr);
    CHECK_FALSE(stage.scene.tweening(vnmaki::kTransitionTag));
  }
  SUBCASE("from the line after it") {
    stage.runtime.advance();
    REQUIRE(stage.text() == "two");
    CHECK(stage.runtime.roll_back());
    CHECK(stage.text() == "one");  // not the transition in between
    CHECK_FALSE(stage.scene.tweening(vnmaki::kTransitionTag));
    stage.runtime.advance();
    CHECK(stage.at_transition());
    CHECK(stage.scene.tweening(vnmaki::kTransitionTag));  // it shows again, live
  }
}

TEST_CASE("script: a restore ends a running transition") {
  Stage stage{std::string{kStory}};
  stage.runtime.advance();
  REQUIRE(stage.scene.tweening(vnmaki::kTransitionTag));
  stage.scene.restore(stage.scene.capture());
  CHECK_FALSE(stage.scene.tweening(vnmaki::kTransitionTag));
}

TEST_CASE("script: the headless runner ends a transition at once and counts it as a stop") {
  const vnmaki::Vfs vfs = game_files(std::string{kStory});
  Scene scene{vfs, kVirtualSize};
  vnmaki::HeadlessOptions options;
  options.runtime.scene = &scene;
  options.runtime.scene_state = &scene;
  const std::vector<vnmaki::ScriptInput> inputs{vnmaki::AdvanceInput{}, vnmaki::AdvanceInput{}};
  const vnmaki::HeadlessResult result = vnmaki::run_headless(vfs, inputs, options);
  CHECK(result.outcome == vnmaki::HeadlessOutcome::kFinished);
  CHECK(result.transcript == std::vector<std::string>{"label start", "narrate one", "show eileen",
                                                      "transition dissolve", "wait @transition",
                                                      "narrate two"});
  CHECK(result.stops == std::vector<vnmaki::ScriptPosition>{
                            {.label = "start", .step = 1, .wait = false},
                            {.label = "start", .step = 2, .wait = true},
                            {.label = "start", .step = 3, .wait = false},
                        });
}

TEST_CASE("script: without a scene a transition is still a step") {
  const vnmaki::Vfs vfs = game_files(std::string{kStory});
  const std::vector<vnmaki::ScriptInput> inputs{vnmaki::AdvanceInput{}, vnmaki::AdvanceInput{}};
  const vnmaki::HeadlessResult result = vnmaki::run_headless(vfs, inputs);
  CHECK(result.outcome == vnmaki::HeadlessOutcome::kFinished);
  CHECK(result.transcript.size() == 6);
}

TEST_CASE("script: a custom transition needs a declared shader with the uniform progress") {
  CHECK(line_error(R"(transition("swirl"))") ==
        "scripts/main.lua:3: unknown transition 'swirl' (the built-in ones are dissolve, fade, "
        "slide_left, slide_right, slide_up, slide_down, wipe_left, wipe_right, wipe_up, "
        "wipe_down and mask; or declare a shader with shader(name, { \"progress\" }))");
}

constexpr std::string_view kQuickShader = R"hlsl(#include "transition.hlsli"

cbuffer Uniforms : register(b0, space3) {
  float progress;
  float amount;
  float2 unused_padding;
};

float4 main(SpriteFragment input) : SV_Target0 {
  return lerp(from_at(input.uv), to_at(input.uv), saturate(progress * amount));
}
)hlsl";

TEST_CASE("script: a custom transition checks its shader and uniforms") {
  if (!vnmaki::shader_compiler_available()) {
    MESSAGE("no run-time shader compiler in this build");
    return;
  }
  const std::string shaders =
      "shader(\"quick\", { \"progress\", \"amount\" })\n"
      "shader(\"plain\", { \"amount\" })\n";
  // Runs `call` as the first line of a label: why the story failed (empty if it did not), and
  // whether the transition runs.
  const auto run = [&](const std::string& call) {
    const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({
        {"scripts/main.lua", shaders + "label(\"start\", function()\n" + call + "\nend)"},
        {"shaders/quick.frag.hlsl", std::string{kQuickShader}},
        {"shaders/plain.frag.hlsl", std::string{kQuickShader}},
    });
    Scene scene{vfs, kVirtualSize};
    vnmaki::ScriptRuntimeOptions options;
    options.scene = &scene;
    options.scene_state = &scene;
    vnmaki::ScriptRuntime runtime{vfs, options};
    runtime.load_scripts();
    runtime.start("start");
    const bool waiting = runtime.state() == vnmaki::RunState::kWaiting;
    return std::pair{waiting ? std::string{} : runtime.error(),
                     waiting && scene.tweening(vnmaki::kTransitionTag)};
  };
  const auto [fine, shown] = run(R"(transition("quick", { uniforms = { amount = 2 } }))");
  CHECK(fine.empty());
  CHECK(shown);
  CHECK(run(R"(transition("plain"))").first ==
        "scripts/main.lua:4: shader 'plain' needs the uniform 'progress' to be a transition");
  CHECK(run(R"(transition("quick", { uniforms = { speed = 1 } }))").first ==
        "scripts/main.lua:4: shader 'quick' has no uniform 'speed'");
  CHECK(run(R"(transition("quick", { uniforms = { progress = 1 } }))").first ==
        "scripts/main.lua:4: the uniform 'progress' is set by the engine");
}

// --- Script: errors --------------------------------------------------------------------------

TEST_CASE("script: bad transition calls name the script line") {
  CHECK(line_error(R"(transition("nonsense"))")
            .starts_with("scripts/main.lua:3: unknown transition"));
  CHECK(line_error(R"(show("lucy", { with = "nonsense" }))")
            .starts_with("scripts/main.lua:3: unknown transition 'nonsense'"));
  CHECK(line_error(R"(hide("eileen", { with = "nonsense" }))")
            .starts_with("scripts/main.lua:3: unknown transition 'nonsense'"));
  CHECK(line_error(R"(scene("bg/room", { with = "nonsense" }))")
            .starts_with("scripts/main.lua:3: unknown transition 'nonsense'"));
  CHECK(line_error("transition(1)") ==
        "scripts/main.lua:3: bad argument #1 to 'transition' (string expected, got number)");
  CHECK(line_error(R"(transition("dissolve", "x"))") ==
        "scripts/main.lua:3: bad argument #2 to 'transition' (table or nil expected, got string)");
  CHECK(line_error(R"(transition("dissolve", { duration = "long" }))") ==
        "scripts/main.lua:3: bad option 'duration' to 'transition' (number expected, got string)");
  CHECK(line_error(R"(transition("dissolve", { duration = -1 }))") ==
        "scripts/main.lua:3: option 'duration' must be a number from 0 to 86400");
  CHECK(line_error(R"(transition("fade", { color = "red" }))") ==
        R"x(scripts/main.lua:3: bad option 'color' to 'transition' ("#rrggbb" expected))x");
  CHECK(line_error(R"(transition("mask"))") ==
        "scripts/main.lua:3: the transition 'mask' needs the option 'mask' (an image name)");
  CHECK(line_error(R"(transition("mask", { mask = "masks/none" }))") ==
        "scripts/main.lua:3: image 'masks/none' not found (no images/masks/none.png, "
        "images/masks/none.jpg or images/masks/none.webp)");
  CHECK(line_error(R"(transition("dissolve", { softness = 1 }))") ==
        "scripts/main.lua:3: option 'softness' does not apply to the transition 'dissolve'");
  CHECK(line_error(R"(show("lucy", { with = 1 }))") ==
        "scripts/main.lua:3: bad option 'with' to 'show' (a transition name, or a table { name, "
        "... } expected)");
  CHECK(line_error(R"(show("lucy", { with = { duration = 1 } }))") ==
        "scripts/main.lua:3: bad option 'with' to 'show' (a transition name, or a table { name, "
        "... } expected)");
  CHECK(line_error(R"(show("lucy", { with = { "dissolve", duration = true } }))") ==
        "scripts/main.lua:3: bad option 'duration' to 'show' (number expected, got boolean)");
}

TEST_CASE("script: a bad with changes nothing") {
  const Stage stage{R"(
label("start", function()
  show("eileen", { with = "nonsense" })
end)
)"};
  REQUIRE(stage.runtime.state() == vnmaki::RunState::kFailed);
  CHECK(stage.scene.find("eileen") == nullptr);
}

}  // namespace
