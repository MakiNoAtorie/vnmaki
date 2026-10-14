// One frame of a scene, composed on a real GPU device: the script of the generated project
// scene_game (tools/generators/render_fixtures.py) shows a background and two sprites, and the
// frame must match the golden image the generator's reference rasterizer drew.

#include "engine/scene/scene.hpp"

#include <filesystem>

#include <doctest/doctest.h>

#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/render/gpu_test_support.hpp"

TEST_CASE("scene: a composed frame shows the background and the positioned sprites") {
  constexpr vnmaki::Size kVirtualSize{.width = 32, .height = 32};
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_FIXTURES_DIR} /
                                                 "render" / "scene_game")));
  vnmaki::JobSystem jobs;
  vnmaki::Scene scene{vfs, kVirtualSize};
  vnmaki::ScriptRuntimeOptions options;
  options.scene = &scene;
  options.scene_state = &scene;
  vnmaki::ScriptRuntime runtime{vfs, options};
  runtime.load_scripts();
  runtime.start("start");
  REQUIRE_MESSAGE(runtime.state() == vnmaki::RunState::kWaiting, runtime.error());
  const vnmaki::TextureId target =
      renderer->create_render_target(kVirtualSize, vnmaki::TextureFilter::kNearest);

  scene.prepare(*renderer, jobs);
  renderer->begin_frame();
  renderer->begin_pass({.target = target, .clear = vnmaki::rgba8(0, 0, 0)});
  scene.draw(*renderer);
  const vnmaki::FrameStats stats = renderer->end_frame();

  CHECK(stats.quads == 3);
  vnmaki::test::check_golden(renderer->read_pixels(target), "scene_frame");
}
