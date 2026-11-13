// The camera on a real GPU device: each label of the generated project camera/game
// (tools/generators/camera_fixtures.py) moves the camera of one scene, and the frame must match
// the golden of the generator's reference rasterizer. The sprite on the UI layer does not move.

#include <filesystem>
#include <string>

#include <doctest/doctest.h>

#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/render/gpu_test_support.hpp"

namespace {

constexpr vnmaki::Size kVirtualSize{.width = 32, .height = 32};
constexpr double kShakeSeconds = 0.3;  // where the shake frame is taken

// Draws the story's `label` after `seconds` of the scene's clock and checks it against the golden
// of the same name.
void check_camera(const std::string& label, double seconds = 0) {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_FIXTURES_DIR} /
                                                 "camera" / "game")));
  vnmaki::JobSystem jobs;
  vnmaki::Scene scene{vfs, kVirtualSize};
  vnmaki::ScriptRuntimeOptions options;
  options.scene = &scene;
  options.scene_state = &scene;
  vnmaki::ScriptRuntime runtime{vfs, options};
  runtime.load_scripts();
  runtime.start(label);
  REQUIRE_MESSAGE(runtime.state() == vnmaki::RunState::kWaiting, runtime.error());
  scene.update(seconds);

  const vnmaki::TextureId target =
      renderer->create_render_target(kVirtualSize, vnmaki::TextureFilter::kNearest);
  scene.prepare(*renderer, jobs);
  renderer->begin_frame();
  renderer->begin_pass({.target = target, .clear = vnmaki::rgba8(0, 0, 0)});
  scene.draw(*renderer, target);
  static_cast<void>(renderer->end_frame());
  INFO(label);
  vnmaki::test::check_golden_in("camera", renderer->read_pixels(target), label);
}

}  // namespace

TEST_CASE("camera: no camera draws the layers as they are") { check_camera("none"); }
TEST_CASE("camera: a pan moves the content the other way") { check_camera("pan"); }
TEST_CASE("camera: a zoom grows the content about the centre") { check_camera("zoom"); }
TEST_CASE("camera: a rotation turns the content about the centre") { check_camera("rotation"); }
TEST_CASE("camera: layers at two depths pan by their parallax") { check_camera("parallax"); }
TEST_CASE("camera: a shake at a fixed time offsets the layers") {
  check_camera("shake", kShakeSeconds);
}
