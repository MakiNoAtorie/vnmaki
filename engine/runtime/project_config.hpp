#pragma once

// project.toml, the per-game configuration (docs/SPEC.md section 19).
//
//   title = "My Novel"            # required
//   version = "1.0.0"             # default "0.0.0"
//   resolution = [1920, 1080]     # width, height; default [1920, 1080]
//   languages = ["en", "ja"]      # the first is the source language;
//                                 # default ["en"]
//   auto_save = ["label", "menu"] # when to auto save (engine/saves): at the first line of
//                                 # each label, at each menu; default [] (never)
//
// Other keys (platforms, plugins, build options) belong to later features and
// are ignored for now.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vnmaki {

class Vfs;

struct Resolution {
  int width = 0;
  int height = 0;

  friend bool operator==(const Resolution&, const Resolution&) = default;
};

/// An event of the story that writes the auto-save slot.
enum class AutoSaveEvent : std::uint8_t {
  kLabel,  ///< The story waits at the first yield point of a label.
  kMenu,   ///< The story waits at a menu.
};

struct ProjectConfig {
  std::string title;
  std::string version;
  Resolution resolution;
  std::vector<std::string> languages;
  std::vector<AutoSaveEvent> auto_save{};
};

/// project.toml is missing, is not valid TOML, or breaks the schema above.
/// The message starts with "project.toml" and, for errors at a place in the
/// file, its line and column ("project.toml:3:1: ...").
class ProjectConfigError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// The virtual path of the project file at the root of a game.
inline constexpr std::string_view kProjectConfigPath = "project.toml";

/// Parses the text of a project.toml. Throws ProjectConfigError.
[[nodiscard]] ProjectConfig parse_project_config(std::string_view toml_text);

/// Reads and parses project.toml from the root of `vfs`. Throws
/// ProjectConfigError.
[[nodiscard]] ProjectConfig load_project_config(const Vfs& vfs);

}  // namespace vnmaki
