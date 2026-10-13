#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace vnmaki::cli {

inline constexpr std::string_view kRunUsage =
    "       vnmaki run [<project-dir>] --headless [--max-steps <n>]\n";

/// `vnmaki run [<project-dir>] --headless [--max-steps <n>]` (docs/SPEC.md
/// section 19). Runs the game in <project-dir> (default: the current
/// directory) from its `start` label with no window, advancing past every
/// line, and prints the transcript (see engine/runtime/headless_runner.hpp)
/// to `out`. Stops with an error after <n> steps (default 100000), so a game
/// that never ends still terminates.
///
/// `args` are the arguments after "run". Returns the process exit code: 0 when
/// the story ends, 1 on a project or script error or at the step limit, 2 on
/// a usage error.
int run_command(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace vnmaki::cli
