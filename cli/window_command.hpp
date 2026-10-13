#pragma once

#include <ostream>
#include <span>
#include <string_view>

namespace vnmaki::cli {

inline constexpr std::string_view kWindowUsage =
    "       vnmaki window [--frames N] [--image FILE]\n";

/// `vnmaki window [--frames N] [--image FILE]`: opens a window and runs the main loop until the
/// window is closed, or for N frames. With --image it shows that PNG or JPEG as a background on
/// the GPU (cli/image_view.hpp); without, the window stays empty. A smoke test of the platform
/// and render layers until the windowed runner exists. `args` excludes "window". Returns the
/// process exit code: 0, 1 when the window, the GPU or the image cannot be used, 2 for bad
/// arguments.
int window_command(std::span<const std::string_view> args, std::ostream& err);

}  // namespace vnmaki::cli
