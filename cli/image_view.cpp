#include "cli/image_view.hpp"

#include <memory>
#include <optional>
#include <string>

#include "engine/jobs/job_system.hpp"
#include "engine/platform/main_loop.hpp"
#include "engine/platform/platform.hpp"
#include "engine/platform/viewport.hpp"
#include "engine/render/image.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/sdl_gpu_renderer.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki::cli {

namespace {

constexpr int kExitFailure = 1;
constexpr Size kVirtualSize = kDefaultWindowSize;

}  // namespace

int run_image_view(SdlPlatform& platform, const std::filesystem::path& image_file,
                   std::optional<std::uint64_t> max_frames, std::ostream& err) {
  Vfs vfs;
  const std::string virtual_path = image_file.filename().generic_string();
  if (!vfs.mount(make_directory_mount(image_file.parent_path().empty()
                                          ? std::filesystem::path{"."}
                                          : image_file.parent_path())) ||
      !vfs.exists(virtual_path)) {
    err << "vnmaki window: no image file at '" << image_file.generic_string() << "'\n";
    return kExitFailure;
  }
  JobSystem jobs;
  JobHandle<Image> pending = load_image(vfs, jobs, virtual_path);

  std::unique_ptr<SdlGpuRenderer> renderer;
  try {
    renderer = SdlGpuRenderer::create_for_window(platform);
  } catch (const RenderError& error) {
    err << "vnmaki window: " << error.what() << '\n';
    return kExitFailure;
  }

  const TextureId canvas = renderer->create_render_target(kVirtualSize, TextureFilter::kLinear);
  std::optional<TextureId> background;
  std::optional<std::string> failure;
  const auto take_image = [&] {
    try {
      background = renderer->create_texture(pending.get(), TextureFilter::kLinear);
    } catch (const ImageError& error) {
      failure = error.what();
    }
  };

  SteadyClock clock;
  MainLoop loop{platform, clock};  // Presenting with vsync paces the loop.
  MainLoopHooks hooks;
  hooks.draw = [&](double /*alpha*/) {
    if (pending.valid() && pending.ready()) {
      take_image();
      if (failure) {
        loop.request_quit();
        return;
      }
    }
    renderer->begin_frame();
    renderer->begin_pass({.target = canvas, .clear = rgba8(0, 0, 0)});
    if (background) {
      renderer->draw(quad_from_rect({.x = 0,
                                     .y = 0,
                                     .width = static_cast<float>(kVirtualSize.width),
                                     .height = static_cast<float>(kVirtualSize.height)}),
                     {.texture = *background});
    }
    renderer->begin_pass({.target = kScreen, .clear = rgba8(0, 0, 0)});
    const Viewport viewport =
        compute_viewport(kVirtualSize, platform.window_metrics().pixels, ScaleMode::kFit);
    renderer->draw(quad_from_rect(viewport.rect), {.texture = canvas});
    static_cast<void>(renderer->end_frame());
  };
  try {
    loop.run(hooks, max_frames);
    // A short run may end before the image arrives; still report whether it loads.
    if (pending.valid()) {
      take_image();
    }
  } catch (const RenderError& error) {
    failure = error.what();
  }
  if (failure) {
    err << "vnmaki window: " << *failure << '\n';
    return kExitFailure;
  }
  return 0;
}

}  // namespace vnmaki::cli
