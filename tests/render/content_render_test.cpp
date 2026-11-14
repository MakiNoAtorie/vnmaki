// The content demo on a real GPU device: the label "content" of templates/default/ is played as
// the host plays it, with a fake clock (Scene::update), and three frames must match the goldens
// that tools/generators/content_fixtures.py drew. That file says what each frame is and why.

#include <cstddef>
#include <filesystem>
#include <string>
#include <variant>

#include <doctest/doctest.h>

#include "engine/audio/audio.hpp"
#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/shader_compiler.hpp"
#include "engine/runtime/project_config.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/render/gpu_test_support.hpp"
#include "tests/support/template_project.hpp"

namespace {

constexpr vnmaki::Size kVirtualSize{.width = 1280, .height = 720};
// Seconds since the clip started, and since the transition started. Binary fractions, so the clock
// adds up exactly.
constexpr double kSlideAt = 0.5;
constexpr double kFlashAt = 0.25;
constexpr std::size_t kEnglish = 0;  // the first option of the language menu

}  // namespace

TEST_CASE("content: the arm at rest, the arm sliding and the flash of the plugin match goldens") {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  // The language menu of the label is in a game with a second language.
  const vnmaki::test::TemplateProject project{"content", {"ja"}};
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(project.path())));
  if (!vnmaki::shader_compiler_available()) {
    // A release build cannot compile the sepia shader of the presentation label, which loads with
    // all scripts; it loads the build's precompiled file.
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
  options.languages = vnmaki::load_project_config(vfs).languages;  // the label asks for a switch
  vnmaki::ScriptRuntime runtime{vfs, options};
  runtime.load_scripts();
  runtime.start("content");
  const vnmaki::TextureId target =
      renderer->create_render_target(kVirtualSize, vnmaki::TextureFilter::kNearest);

  // Ends the waits (the clip) as a click does, up to the next say or menu. A transition is left to
  // run when `keep_transition` is set.
  const auto settle = [&](bool keep_transition = false) {
    while (const auto* wait = std::get_if<vnmaki::WaitRequest>(runtime.request())) {
      if (keep_transition && wait->tag == vnmaki::kTransitionTag) {
        return;
      }
      scene.finish_tweens(wait->tag);
      runtime.advance();
    }
    REQUIRE_MESSAGE(runtime.state() == vnmaki::RunState::kWaiting, runtime.error());
  };
  const auto draw = [&] {
    scene.prepare(*renderer, jobs);
    renderer->begin_frame();
    renderer->begin_pass({.target = target, .clear = vnmaki::rgba8(0, 0, 0)});
    scene.draw(*renderer, target);
    static_cast<void>(renderer->end_frame());
  };
  const auto check = [&](const std::string& golden) {
    draw();
    INFO(golden);
    vnmaki::test::check_golden_in("content", renderer->read_pixels(target), golden);
  };
  const auto next = [&] {
    runtime.advance();
    settle();
  };

  settle();  // "This is the content demo."
  next();    // "First the language."
  next();    // The language menu.
  REQUIRE(std::holds_alternative<vnmaki::MenuRequest>(*runtime.request()));
  runtime.choose(kEnglish);
  settle();  // "The story is the same in both languages."
  next();    // The caption line; the chime plays.
  next();    // "This is a 3D model": the arm stands at rest.
  check("content_rest");
  next();  // The clip "slide" starts.
  scene.update(kSlideAt);
  check("content_slide");
  next();  // The wait for the clip ends it, and the story reaches "A plugin adds the transition".
  REQUIRE(std::holds_alternative<vnmaki::SayRequest>(*runtime.request()));
  draw();             // the frame that the player saw before the step: the classroom, nothing else
  runtime.advance();  // the new scene and the sprite show with the plugin transition
  settle(true);
  const auto* wait = std::get_if<vnmaki::WaitRequest>(runtime.request());
  REQUIRE(wait != nullptr);
  CHECK(wait->tag == vnmaki::kTransitionTag);
  scene.update(kFlashAt);
  check("content_flash");
}
