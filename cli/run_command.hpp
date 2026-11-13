#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace vnmaki::cli {

inline constexpr std::string_view kRunUsage =
    "       vnmaki run [<project-dir>] [--headless [--max-steps <n>] | --frames <n>]"
    " [--data-dir <path>] [--profile <file.csv>]\n";

/// `vnmaki run [<project-dir>] [--headless [--max-steps <n>] | --frames <n>] [--data-dir <path>]`
/// (docs/SPEC.md section 19). Runs the game in <project-dir> (default: the
/// current directory) from its `start` label.
///
/// With --headless there is no window: it advances past every line and prints
/// the transcript (see engine/runtime/headless_runner.hpp) to `out`. It stops
/// with an error after <n> steps (default 100000), so a game that never ends
/// still terminates.
///
/// Without it, the game plays in a window (cli/story_window.hpp), for <n>
/// frames at most.
///
/// A <project-dir> that has a `game.vnpak` is a built game (docs/SPEC.md section 19): the project
/// is that pack and the `patch-*.vnpak` files next to it (engine/vfs/pak_mount.hpp), not the loose
/// files. A pak that cannot be read gives exit code 1.
///
/// Saves and the persistent file go to the user-data directory of the game's title, or to
/// <path> with --data-dir (in a window and headless). Tests use it to leave the user's data alone.
///
/// With --profile <file.csv> (a window only, in every build) the run writes one CSV row for each
/// frame to <file.csv> and the asset loads to <file>_loads.csv (engine/tools/profiler.hpp).
///
/// `args` are the arguments after "run". Returns the process exit code: 0 when
/// the story ends, 1 on a project or script error or at the step limit, 2 on
/// a usage error.
int run_command(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace vnmaki::cli
