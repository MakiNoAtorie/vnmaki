#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace vnmaki::cli {

inline constexpr std::string_view kDebugUsage =
    "       vnmaki debug [<project-dir>] [--headless] [--data-dir <path>]\n";

/// `vnmaki debug [<project-dir>] [--headless] [--data-dir <path>]` (docs/SPEC.md section 18).
/// Runs the game in <project-dir> (default: the current directory) under the script debugger.
/// It speaks the Debug Adapter Protocol on stdin and stdout (Content-Length framing; nothing
/// else is written to stdout) and the story starts when the client sends `configurationDone`.
/// It opens the game window (cli/story_window.hpp). The window keeps drawing while the story is
/// stopped, and closes when the client disconnects or ends stdin.
///
/// --headless opens no window, for tests and for projects with no GPU. The story then advances by
/// itself, like `vnmaki run --headless`, until it stops at a breakpoint or ends, and a goto leaves
/// it waiting until the client continues or steps. Saves and `persistent` go to --data-dir, or
/// to the user-data directory of the game's title.
///
/// The command needs a build with the dev tools and Windows. `args` are the arguments after
/// "debug". Returns the process exit code: 0 when the session ends, 1 on a project or script
/// error or when the build has no debugger, 2 on a usage error.
int debug_command(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace vnmaki::cli
