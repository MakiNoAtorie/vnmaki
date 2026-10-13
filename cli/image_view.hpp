#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>

#include "engine/platform/sdl_platform.hpp"

namespace vnmaki::cli {

/// Shows the PNG or JPEG `image_file` as a background in `platform`'s window until the window is
/// closed, or for `max_frames` frames. The file's directory is mounted as the VFS root; the image
/// is read through the VFS and decoded on the job system while the window already runs, then
/// drawn over the whole virtual canvas (1280x720), which is scaled into the window with
/// letterboxing. Returns the process exit code: 0, or 1 (after a message on `err`) when the image
/// or the GPU cannot be used.
int run_image_view(SdlPlatform& platform, const std::filesystem::path& image_file,
                   std::optional<std::uint64_t> max_frames, std::ostream& err);

}  // namespace vnmaki::cli
