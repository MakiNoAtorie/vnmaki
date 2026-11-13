// The example C++ plugin on a real GPU device (plugins/example/): the project plugin/game
// (tools/generators/plugin_fixtures.py) shows the `example_badge` displayable and a `.ppm` picture
// that the plugin's image loader decodes. The frame must match the generator's golden.

#include <filesystem>

#include <doctest/doctest.h>

#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/plugin/plugin.hpp"
#include "engine/render/image.hpp"
#include "engine/render/renderer.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"
#include "plugins/example/example_plugin.hpp"
#include "tests/render/gpu_test_support.hpp"

namespace {

constexpr vnmaki::Size kVirtualSize{.width = 32, .height = 32};

// The image formats are process-wide: the next test must not see the plugin's.
struct FormatGuard {
  FormatGuard() = default;
  FormatGuard(const FormatGuard&) = delete;
  FormatGuard& operator=(const FormatGuard&) = delete;
  FormatGuard(FormatGuard&&) = delete;
  FormatGuard& operator=(FormatGuard&&) = delete;
  ~FormatGuard() { vnmaki::clear_image_formats(); }
};

}  // namespace

TEST_CASE("plugin: the example displayable and image format draw the golden frame") {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  const FormatGuard formats;
  vnmaki::PluginContext plugins;
  vnmaki::register_plugin_example(plugins);

  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_FIXTURES_DIR} /
                                                 "plugin" / "game")));
  vnmaki::JobSystem jobs;
  vnmaki::Scene scene{vfs, kVirtualSize};
  scene.set_plugins(&plugins);
  vnmaki::ScriptRuntimeOptions options;
  options.scene = &scene;
  options.scene_state = &scene;
  options.plugins = &plugins;
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

  CHECK(stats.quads == 2);
  vnmaki::test::check_golden_in("plugin", renderer->read_pixels(target), "plugin_frame");
}
