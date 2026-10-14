#pragma once

#include <cstdint>
#include <optional>
#include <ostream>
#include <string_view>

#include "engine/runtime/project_config.hpp"

namespace vnmaki {
class Vfs;
}

namespace vnmaki::cli {

/// The VFS path of the font the say box uses. A project must provide it; the engine bundles no
/// font.
inline constexpr std::string_view kDefaultFontPath = "fonts/default.ttf";

/// Plays the game in `vfs` in a window: runs its `start` label, draws the scene and the say box
/// each frame, and advances on a click, Enter, Space or the gamepad's south button (first
/// finishing the typewriter reveal). The window closes once the story ends, or after `max_frames`
/// frames. Returns the process exit code: 0, or 1 (after a message on `err`) when the project,
/// its font, a script or the GPU cannot be used or a script fails while running.
int run_story_window(const Vfs& vfs, const ProjectConfig& config,
                     std::optional<std::uint64_t> max_frames, std::ostream& err);

}  // namespace vnmaki::cli
