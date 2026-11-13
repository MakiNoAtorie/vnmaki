// Transitions on a real GPU device: each label of the generated project transition/game
// (tools/generators/transition_fixtures.py) changes the scene with one built-in transition (or a
// custom shader). The frames at 0, 0.25, 0.5 and 1 of the one second the transition takes, on a
// fake clock, must match the goldens of the generator's reference rasterizer. The sprite on the
// UI layer is drawn over every frame and is not mixed.

#include <array>
#include <filesystem>
#include <string>
#include <utility>
#include <variant>

#include <doctest/doctest.h>

#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/shader_compiler.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/render/gpu_test_support.hpp"

namespace {

constexpr vnmaki::Size kVirtualSize{.width = 32, .height = 32};
constexpr double kQuarter = 0.25;
constexpr double kHalf = 0.5;
constexpr double kDuration = 1.0;
constexpr double kAfter = 0.1;

// The clock when each golden is taken, and the golden's stage name.
constexpr std::array<std::pair<double, const char*>, 4> kStages{{
    {0.0, "0"},
    {kQuarter, "25"},
    {kHalf, "50"},
    {kDuration, "100"},
}};

void check_transition(const std::string& label) {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  if (!vnmaki::shader_compiler_available()) {
    MESSAGE("no run-time shader compiler in this build");
    return;
  }
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_FIXTURES_DIR} /
                                                 "transition" / "game")));
  vnmaki::JobSystem jobs;
  vnmaki::Scene scene{vfs, kVirtualSize};
  vnmaki::ScriptRuntimeOptions options;
  options.scene = &scene;
  options.scene_state = &scene;
  vnmaki::ScriptRuntime runtime{vfs, options};
  runtime.load_scripts();
  runtime.start(label);
  REQUIRE_MESSAGE(runtime.state() == vnmaki::RunState::kWaiting, runtime.error());
  const vnmaki::TextureId target =
      renderer->create_render_target(kVirtualSize, vnmaki::TextureFilter::kNearest);

  const auto frame = [&] {
    scene.prepare(*renderer, jobs);
    renderer->begin_frame();
    renderer->begin_pass({.target = target, .clear = vnmaki::rgba8(0, 0, 0)});
    scene.draw(*renderer, target);
    static_cast<void>(renderer->end_frame());
    return renderer->read_pixels(target);
  };
  const auto check = [&](const char* stage) {
    INFO(label << " at stage " << stage);
    vnmaki::test::check_golden_in("transition", frame(), label + "_" + stage);
  };

  static_cast<void>(frame());  // the frame the player saw before the step
  runtime.advance();           // the scene changes and the transition starts
  REQUIRE_MESSAGE(runtime.state() == vnmaki::RunState::kWaiting, runtime.error());
  const auto* wait = std::get_if<vnmaki::WaitRequest>(runtime.request());
  REQUIRE(wait != nullptr);
  CHECK(wait->tag == vnmaki::kTransitionTag);

  double elapsed = 0;
  for (const auto& [seconds, stage] : kStages) {
    scene.update(seconds - elapsed);
    elapsed = seconds;
    check(stage);
  }
  // The transition ends at the next update, and the plain scene is what its end showed.
  CHECK_FALSE(scene.tweening(vnmaki::kTransitionTag));
  scene.update(kAfter);
  check("100");
}

}  // namespace

TEST_CASE("transition: dissolve") { check_transition("dissolve"); }
TEST_CASE("transition: fade through a colour") { check_transition("fade"); }
TEST_CASE("transition: slide left") { check_transition("slide_left"); }
TEST_CASE("transition: slide right") { check_transition("slide_right"); }
TEST_CASE("transition: slide up") { check_transition("slide_up"); }
TEST_CASE("transition: slide down") { check_transition("slide_down"); }
TEST_CASE("transition: wipe left with a soft edge") { check_transition("wipe_left"); }
TEST_CASE("transition: wipe right") { check_transition("wipe_right"); }
TEST_CASE("transition: wipe up with a soft edge") { check_transition("wipe_up"); }
TEST_CASE("transition: wipe down") { check_transition("wipe_down"); }
TEST_CASE("transition: an image mask") { check_transition("mask"); }
TEST_CASE("transition: a custom shader by name") { check_transition("custom"); }
