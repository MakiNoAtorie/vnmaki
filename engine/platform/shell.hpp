#pragma once

// The few OS shell services the launcher needs. They are implemented for Windows; on other
// systems they do nothing and report failure.

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace vnmaki {

/// The path of the running executable, or an empty path if the OS gives none.
[[nodiscard]] std::filesystem::path executable_path();

/// The command line of the process as UTF-8 arguments, the program name first (`argv` is in the ANSI
/// code page, so a name outside it is lost there). Empty where there is no such call; the caller
/// then uses `argv`.
[[nodiscard]] std::vector<std::string> command_line_utf8();

/// Starts `exe` with `args` as a new process and returns at once; the process goes on if the caller
/// ends. Returns false if it cannot start.
[[nodiscard]] bool spawn_process(const std::filesystem::path& exe,
                                 std::span<const std::filesystem::path> args);

/// Shows `folder` in the file manager. Returns false if it cannot.
[[nodiscard]] bool open_folder(const std::filesystem::path& folder);

}  // namespace vnmaki
