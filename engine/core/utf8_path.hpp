#pragma once

// Text in the engine is UTF-8, but std::filesystem reads a narrow string in the ANSI code page on
// Windows (a name outside it becomes '?'). Every path that comes from or goes to UTF-8 text
// (arguments, SDL, project.toml, ImGui, recent.txt) must pass through these two.

#include <filesystem>
#include <string>
#include <string_view>

namespace vnmaki {

[[nodiscard]] inline std::filesystem::path utf8_path(std::string_view utf8) {
  return std::filesystem::path{std::u8string{utf8.begin(), utf8.end()}};
}

/// Throws std::system_error if a Windows name has no UTF-8 form (an unpaired surrogate).
[[nodiscard]] inline std::string to_utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string{text.begin(), text.end()};
}

/// The same with '/' separators. `path.string()`, `generic_string()` and `stream << path` throw on
/// Windows for a name outside the ANSI code page, so messages and virtual paths use these.
[[nodiscard]] inline std::string to_generic_utf8(const std::filesystem::path& path) {
  const std::u8string text = path.generic_u8string();
  return std::string{text.begin(), text.end()};
}

}  // namespace vnmaki
