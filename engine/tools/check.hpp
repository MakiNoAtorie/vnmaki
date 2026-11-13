#pragma once

// `vnmaki check` (docs/SPEC.md sections 18 and 19): lints the scripts and `project.toml` of a game
// without running it. It reuses the scan and the story graph (story_graph.hpp), the text markup
// parser, `parse_project_config` and the asset lookup of the scene and the audio.
//
// Codes. Errors: `lua-syntax`, `unknown-label`, `bad-text-tag`, `missing-image`, `missing-audio`,
// `missing-movie`, `po-syntax` and `bad-translation-tag` (a translation in `translations/*.po`
// that is in use, not fuzzy, whose text tags do not parse). Warnings: `unreachable-label`, `dead-end`,
// `dynamic-edge`, `project-config` and `non-literal-string` (the text of `say` or `narrate`, a
// `menu` option or a screen `text` or `button` that is not a string literal, so `vnmaki
// extract-strings` skips it).

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/vfs/vfs.hpp"

namespace vnmaki {

enum class Severity : std::uint8_t { kError, kWarning };

struct Finding {
  std::string file{};      ///< The Vfs path, e.g. "scripts/main.lua" or "project.toml".
  std::uint32_t line = 0;  ///< From 1; 0 when the finding has no line.
  Severity severity = Severity::kError;
  std::string code{};
  std::string message{};

  friend bool operator==(const Finding&, const Finding&) = default;
};

/// Checks the game in `vfs`. Sorted by file, then line; findings of one place keep the order of
/// the checks. A script with a Lua syntax error is still scanned for the other findings.
[[nodiscard]] std::vector<Finding> check_project(const Vfs& vfs);

[[nodiscard]] bool has_errors(std::span<const Finding> findings);

/// "error" or "warning".
[[nodiscard]] std::string_view severity_name(Severity severity);

/// CSV (RFC 4180, LF line ends) with the header `file,line,severity,code,message`, then a row for
/// each finding. The line is empty when 0.
[[nodiscard]] std::string findings_to_csv(std::span<const Finding> findings);

}  // namespace vnmaki
