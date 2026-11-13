// Game shaders on a real GPU device: the script of the generated project shader/game
// (tools/generators/shader_fixtures.py) puts a sepia shader on a sprite and on a layer and
// tweens its uniform. The frames must match the goldens the generator's reference rasterizer
// drew.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

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
constexpr double kHalfSecond = 0.5;  // Half way through the one second tween.

// The scene of one label of shader/game, drawn at the start and half way through its tween.
void check_label(const std::string& label, const std::string& golden_prefix,
                 std::uint32_t expected_quads) {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_FIXTURES_DIR} /
                                                 "shader" / "game")));
  if (!vnmaki::shader_compiler_available()) {
    // A release build: the shader loads from the file that the build precompiled, so these goldens
    // also check that a precompiled shader makes a pipeline and draws the same.
    REQUIRE(vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_PRECOMPILED_DIR})));
  }
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
    return renderer->end_frame();
  };

  CHECK(frame().quads == expected_quads);
  vnmaki::test::check_golden_in("shader", renderer->read_pixels(target), golden_prefix + "_low");

  runtime.advance();  // Starts the tween.
  REQUIRE_MESSAGE(runtime.state() == vnmaki::RunState::kWaiting, runtime.error());
  scene.update(kHalfSecond);
  CHECK(frame().quads == expected_quads);
  vnmaki::test::check_golden_in("shader", renderer->read_pixels(target), golden_prefix + "_high");
}

}  // namespace

TEST_CASE("shader: a sepia shader on a sprite, before and during a uniform tween") {
  constexpr std::uint32_t kQuads = 3;  // The backdrop and the two sprites.
  check_label("start", "sepia_sprite", kQuads);
}

TEST_CASE("shader: a sepia shader on a layer, before and during a uniform tween") {
  // The backdrop and a sprite; then the other sprite in the layer's target, and the target.
  constexpr std::uint32_t kQuads = 4;
  check_label("layer", "sepia_layer", kQuads);
}

TEST_CASE("shader: a shader that does not compile stops the scripts with its file and line") {
  if (!vnmaki::shader_compiler_available()) {
    MESSAGE("no run-time shader compiler in this build");
    return;
  }
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_FIXTURES_DIR} /
                                                 "shader" / "broken")));
  vnmaki::Scene scene{vfs, kVirtualSize};
  vnmaki::ScriptRuntimeOptions options;
  options.scene = &scene;
  options.scene_state = &scene;
  vnmaki::ScriptRuntime runtime{vfs, options};
  try {
    runtime.load_scripts();
    FAIL("the scripts should not load");
  } catch (const std::exception& error) {
    const std::string message = error.what();
    INFO(message);
    CHECK(message.find("scripts/start.lua:1") != std::string::npos);
    CHECK(message.find("shaders/broken.frag.hlsl:5") != std::string::npos);
    CHECK(message.find("missing_name") != std::string::npos);
  }
}
