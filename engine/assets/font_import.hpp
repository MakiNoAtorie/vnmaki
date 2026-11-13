#pragma once

// The font importer (docs/SPEC.md section 19): a copy of a font for each language that has only the
// glyphs the language needs (HarfBuzz subset). A language needs ASCII and the characters of the
// strings of the game for it. The subset keeps the font's tables for layout, so shaping works.

#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "engine/runtime/project_config.hpp"
#include "engine/tools/story_graph.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki {

inline constexpr char kFontImporter[] = "font-subset";
inline constexpr std::uint32_t kFontImporterVersion = 1;

/// Printable ASCII, 0x20 to 0x7E.
[[nodiscard]] std::set<char32_t> ascii_codepoints();

/// The characters that `language` needs, besides ASCII:
///   - every string literal of the scripts (a language falls back to the source text), with
///     `\u{...}` escapes decoded;
///   - the title of the project;
///   - the translations of `translations/<language>.po` (not for the source language, which has no
///     file).
/// Throws std::runtime_error ("translations/ja.po:5: ...") for a `.po` file that cannot be read.
[[nodiscard]] std::set<char32_t> language_codepoints(const Vfs& vfs, const ProjectConfig& config,
                                                     std::span<const ScriptSource> scripts,
                                                     const std::string& language);

/// The codepoints as a list of decimal numbers, in order; the cache key uses it.
[[nodiscard]] std::string codepoints_text(const std::set<char32_t>& codepoints);

struct FontSubset {
  std::vector<std::byte> font{};
  std::uint32_t glyph_count = 0;  ///< Of the subset, with `.notdef`.
};

/// Keeps the glyphs of `codepoints` (and what they need) of a TrueType or OpenType font. Throws
/// std::runtime_error if the data is not a font or cannot be subset.
[[nodiscard]] FontSubset subset_font(std::span<const std::byte> font,
                                     const std::set<char32_t>& codepoints);

}  // namespace vnmaki
