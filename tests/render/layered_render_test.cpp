// A layered sprite composed on a real GPU device: the script of the generated project
// layered/game (tools/generators/layered_fixtures.py) shows one, and then changes its face. The
// frames must match the goldens the generator's reference rasterizer drew.

#include <cstdint>
#include <filesystem>

#include <doctest/doctest.h>

#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/render/gpu_test_support.hpp"

TEST_CASE("layered: a composed sprite and the middle of a crossfade match the goldens") {
  constexpr vnmaki::Size kVirtualSize{.width = 32, .height = 32};
  constexpr double kFadeStep = 0.1;            // 0.4 of the 0.25 s crossfade.
  constexpr std::uint32_t kComposedQuads = 4;  // The backdrop, the base, the outfit, the face.
  constexpr std::uint32_t kFadingQuads = 5;    // And the new face over the old one.
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_FIXTURES_DIR} /
                                                 "layered" / "game")));
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

  const auto frame = [&] {
    scene.prepare(*renderer, jobs);
    renderer->begin_frame();
    renderer->begin_pass({.target = target, .clear = vnmaki::rgba8(0, 0, 0)});
    scene.draw(*renderer);
    return renderer->end_frame();
  };

  CHECK(frame().quads == kComposedQuads);
  vnmaki::test::check_golden_in("layered", renderer->read_pixels(target), "layered_composed");

  runtime.advance();
  REQUIRE_MESSAGE(runtime.state() == vnmaki::RunState::kWaiting, runtime.error());
  scene.update(kFadeStep);
  CHECK(frame().quads == kFadingQuads);
  vnmaki::test::check_golden_in("layered", renderer->read_pixels(target), "layered_crossfade");
}
