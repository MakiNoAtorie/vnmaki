#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

#include "engine/runtime/project_config.hpp"

namespace vnmaki {
class DapChannel;
class Vfs;
}  // namespace vnmaki

namespace vnmaki::cli {

/// The VFS path of the font the say box uses. A project must provide it; the engine bundles no
/// font.
inline constexpr std::string_view kDefaultFontPath = "fonts/default.ttf";

/// Plays the game in `vfs` in a window: runs its `start` label, draws the scene and the say box
/// each frame, and advances on a click, Enter, Space or the gamepad's south button (first
/// finishing the typewriter reveal, or the tweens a `wait` waits for). The mouse wheel up, Page Up
/// and the gamepad's left shoulder button roll the story back (ScriptRuntime::roll_back()).
/// Tweens move with the main-loop clock. The window closes once the story ends, or after
/// `max_frames` frames. Saves and `persistent` go to `data_directory`, by default the user-data
/// directory of the game's title. Returns the process exit code: 0, or 1 (after a message on `err`)
/// when the project, its font, a script or the GPU cannot be used or a script fails while running.
/// In a dev build, `project_dir` (the folder that `vfs` serves) is watched, and changed scripts,
/// images and shaders are loaded again while the game runs (engine/tools/hot_reload.hpp).
/// With `profile_csv`, the profiler (engine/tools/profiler.hpp) keeps every frame and writes the
/// file and its `_loads` twin when the window closes. Dev builds always profile, for the overlay.
/// With `dap` (`vnmaki debug`, dev builds only) the story starts when the DAP client has set its
/// breakpoints, and the debugger (engine/tools/debugger.hpp) reads the client's requests through
/// it. While the story is stopped the window goes on drawing and handling OS events, and the
/// game gets no input. The window closes when the client disconnects.
/// With `language` the game starts in that language (a code of `languages` in project.toml) and the
/// preference `prefs.language` is set to it; otherwise the preference decides (docs/SPEC.md section
/// 16).
int run_story_window(const Vfs& vfs, const std::filesystem::path& project_dir,
                     const ProjectConfig& config, std::optional<std::uint64_t> max_frames,
                     const std::optional<std::filesystem::path>& data_directory, std::ostream& err,
                     const std::optional<std::filesystem::path>& profile_csv = {},
                     DapChannel* dap = nullptr,
                     const std::optional<std::string>& language = std::nullopt);

}  // namespace vnmaki::cli
