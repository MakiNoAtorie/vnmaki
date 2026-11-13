// A sprite sheet frame on a real GPU device: the script of the generated project
// frame_anim/game (tools/generators/frame_anim_fixtures.py) shows an animation made from a sheet.
// The frame at 0.8 s must match the golden the generator's reference rasterizer drew.

#include <filesystem>

#include <doctest/doctest.h>

#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/render/gpu_test_support.hpp"

TEST_CASE("frame animation: a frame of a sprite sheet matches the golden") {
  constexpr vnmaki::Size kVirtualSize{.width = 32, .height = 32};
  constexpr double kPlayed = 0.8;  // Frame 3 of 4 frames per second.
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_FIXTURES_DIR} /
                                                 "frame_anim" / "game")));
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

  scene.update(kPlayed);
  scene.prepare(*renderer, jobs);
  renderer->begin_frame();
  renderer->begin_pass({.target = target, .clear = vnmaki::rgba8(0, 0, 0)});
  scene.draw(*renderer);
  CHECK(renderer->end_frame().quads == 2);  // The backdrop and the frame.
  vnmaki::test::check_golden_in("frame_anim", renderer->read_pixels(target), "frame_anim_sheet");
}
