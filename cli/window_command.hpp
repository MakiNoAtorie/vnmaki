#pragma once

#include <ostream>
#include <span>
#include <string_view>

namespace vnmaki::cli {

inline constexpr std::string_view kWindowUsage = "       vnmaki window [--frames N]\n";

/// `vnmaki window [--frames N]`: opens an empty window and runs the main loop until the window
/// is closed, or for N frames. A smoke test of the platform layer until the windowed runner
/// exists. `args` excludes "window". Returns the process exit code: 0, 1 when the window cannot
/// open, 2 for bad arguments.
int window_command(std::span<const std::string_view> args, std::ostream& err);

}  // namespace vnmaki::cli
