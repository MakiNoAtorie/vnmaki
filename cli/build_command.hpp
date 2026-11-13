#pragma once

#include <iosfwd>
#include <optional>
#include <span>
#include <string_view>

namespace vnmaki::cli {

inline constexpr std::string_view kBuildUsage =
    "       vnmaki build windows <dir> --out <folder> [--runner <vnmaki.exe>]\n";

/// `vnmaki build windows <dir> --out <folder> [--runner <vnmaki.exe>]` (docs/SPEC.md section 19):
/// imports the project <dir> (the Importer, so it needs a build with the shader compiler when the
/// project has game shaders) and writes the game to <folder>:
///   - `game.vnpak`: every file of the project in its imported form (the `.opus` file in place of
///     the `.wav`, the `.vnshader` file in place of the `.hlsl`, the font subsets in place of the
///     font), without the folders that start with '.', and without <folder> if it is inside <dir>, and without the `game.vnpak`, `patch-*.vnpak`, `.exe` and
///     `.dll` files at the top of <dir> (an earlier build); <folder> as <dir> itself is an error;
///   - `<title>.exe`: a copy of the release runner, and all `.dll` files of its folder.
/// The runner is `--runner`, else the `vnmaki.exe` of the sibling folder `build/release/cli` of the
/// folder of the running executable (`build/debug/cli`). It plays `game.vnpak` when that is next to it (see
/// `build_or_packed_command`). A problem of the import (a file that fails) stops the build.
///
/// `args` are the arguments after "build". Returns the process exit code: 0, 1 when the project
/// cannot be imported or packed or there is no runner, 2 on a usage error.
int build_command(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

/// The two things `main` hands over: `vnmaki build ...` is build_command. A runner that has a
/// `game.vnpak` in its folder, started with no arguments or only options (`--headless`,
/// `--frames 5`, `--data-dir x`), is `vnmaki run <its folder> <options>`, so a player starts the
/// game by its `.exe` alone. Returns nothing for any other arguments.
[[nodiscard]] std::optional<int> build_or_packed_command(std::span<const std::string_view> args,
                                                         std::ostream& out, std::ostream& err);

}  // namespace vnmaki::cli
