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
//   gallery = ["cg/beach"]        # image names (as for `show`) of the gallery; showing one
//                                 # unlocks it (`unlock("cg", name)`); default []
//   rollback_depth = 100          # how many steps the player can roll back; 0 turns rollback
//                                 # off; default 100 (kDefaultRollbackDepth)
//   backlog_length = 100          # how many lines the backlog shows at most, 0 to 10000; it
//                                 # cannot hold more than rollback_depth; default 100
//
//   [fonts.fallback]              # for a language: the fonts (virtual paths)
//   ja = ["fonts/jp.otf"]         # that supply characters the text's font
//                                 # lacks, in the order to try; default none
//
//   [text_direction]              # for a language: "ltr" or "rtl"; default "ltr". Right to
//   ar = "rtl"                    # left reverses the order of the children of an hbox
//
//   [vertical]                    # for a language: true sets the say box and the NVL screen in
//   ja = true                     # vertical text (columns, right to left); default false
//
//   [styles]                      # for a language: the style override file, run after the
//   ja = "styles/ja_look.lua"     # game's styles; default styles/<language>.lua if it exists
//
// Other keys (platforms, plugins, build options) belong to later features and
// are ignored for now.

#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "engine/l10n/text_direction.hpp"
#include "engine/runtime/script_runtime.hpp"

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
  /// The font fallback chain of each language that has one. Every key is in
  /// `languages`. Load the fonts into FontSet::fallbacks (engine/text/text.hpp).
  std::map<std::string, std::vector<std::string>> font_fallbacks;
  /// The text direction of each language that has one in project.toml, and the style override
  /// file of each language that has one. Every key is in `languages`.
  std::map<std::string, TextDirection> text_directions{};
  std::map<std::string, std::string> language_styles{};
  /// The languages whose story text is vertical (`[vertical]`). Every key is in `languages`.
  std::map<std::string, bool> vertical{};
  std::vector<AutoSaveEvent> auto_save{};
  std::vector<std::string> gallery{};
  std::size_t rollback_depth = kDefaultRollbackDepth;
  std::size_t backlog_length = kDefaultBacklogLength;
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
