// The presentation demo on a real GPU device: the label "presentation" of templates/default/ is
// played as the host plays it, with a fake clock (Scene::update), and three frames must match the
// goldens that tools/generators/presentation_fixtures.py drew. That file says what each frame is.

#include <filesystem>
#include <string>
#include <variant>

#include <doctest/doctest.h>

#include "engine/audio/audio.hpp"
#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/shader_compiler.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/render/gpu_test_support.hpp"

namespace {

constexpr vnmaki::Size kVirtualSize{.width = 1280, .height = 720};
// Seconds since the sprite was shown. Binary fractions, so the clock adds up exactly.
constexpr double kFadeAt = 0.125;
constexpr double kCameraAt = 1.9375;
constexpr double kSepiaAt = 2.4375;

}  // namespace

TEST_CASE("presentation: the crossfade, the camera and blink, and the sepia layer match goldens") {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(
      vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_TEMPLATES_DIR} / "default")));
  if (!vnmaki::shader_compiler_available()) {
    // A release build cannot compile the sepia shader; it loads the build's precompiled file.
    REQUIRE(vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_PRECOMPILED_DIR})));
  }
  vnmaki::JobSystem jobs;
  vnmaki::Scene scene{vfs, kVirtualSize};
  vnmaki::Audio audio{vfs};
  scene.set_voice_level(&audio);
  vnmaki::CompositeSceneState state({&scene, &audio});
  vnmaki::ScriptRuntimeOptions options;
  options.scene = &scene;
  options.audio = &audio;
  options.scene_state = &state;
  vnmaki::ScriptRuntime runtime{vfs, options};
  runtime.load_scripts();
  runtime.start("presentation");
  const vnmaki::TextureId target =
      renderer->create_render_target(kVirtualSize, vnmaki::TextureFilter::kNearest);

  // Ends the waits (the dissolve, `wait("@camera")`) as a click does, up to the next say.
  const auto settle = [&] {
    while (const auto* wait = std::get_if<vnmaki::WaitRequest>(runtime.request())) {
      scene.finish_tweens(wait->tag);
      runtime.advance();
    }
    REQUIRE_MESSAGE(runtime.state() == vnmaki::RunState::kWaiting, runtime.error());
    REQUIRE(std::holds_alternative<vnmaki::SayRequest>(*runtime.request()));
  };
  double clock = 0;
  const auto check = [&](double seconds, const std::string& golden) {
    scene.update(seconds - clock);
    clock = seconds;
    scene.prepare(*renderer, jobs);
    renderer->begin_frame();
    renderer->begin_pass({.target = target, .clear = vnmaki::rgba8(0, 0, 0)});
    scene.draw(*renderer, target);
    static_cast<void>(renderer->end_frame());
    INFO(golden);
    vnmaki::test::check_golden_in("presentation", renderer->read_pixels(target), golden);
  };

  settle();  // "This is the presentation demo."
  runtime.advance();
  settle();  // The new face fades in.
  check(kFadeAt, "presentation_face");
  runtime.advance();
  settle();  // The camera pans and shakes.
  check(kCameraAt, "presentation_camera");
  runtime.advance();
  settle();  // The sepia shader fades in.
  check(kSepiaAt, "presentation_sepia");
}
