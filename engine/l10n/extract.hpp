#pragma once

// String extraction for translators (docs/SPEC.md section 16). It reads the literal strings of
// `say`, `narrate`, `menu` and the text nodes of screens from the scripts, and keeps the `.po`
// file of each language up to date.
//
// The key of a string (its `msgctxt`) is `<label>/<hash>`, or `screen/<name>/<hash>` for a screen.
// The hash is 8 hex digits of FNV-1a over the speaker, a NUL byte and the source text. The
// speaker is the first argument of `say` as written (`eileen`, or the text of a string literal).

#include <span>
#include <string>
#include <string_view>

#include "engine/l10n/po.hpp"
#include "engine/tools/story_graph.hpp"

namespace vnmaki {

inline constexpr std::string_view kTranslationsDir = "translations";
inline constexpr std::string_view kTemplatePath = "translations/template.pot";

/// "<scope>/<hash>" (see above). `scope` is a label name or `screen/<name>`.
[[nodiscard]] std::string translation_key(std::string_view scope, std::string_view speaker,
                                          std::string_view text);

/// "translations/<language>.po".
[[nodiscard]] std::string translation_path(std::string_view language);

/// The template: a header, then an entry for each literal string, in the order of the scripts (by
/// path) and of the lines. A text that two lines share has one entry with a reference for each
/// line. Empty strings are not extracted. `msgid` is the source text and `msgstr` is empty. Notes
/// (`#.`) give the speaker, and references (`#:`) give `file:line`.
[[nodiscard]] PoFile extract_template(std::span<const ScriptSource> scripts);

/// The `.po` file of `language` after a new extraction. `old` is the file as it was (empty for a
/// new language).
///   - An entry of the template keeps the translation, flags and notes of the entry of `old` with
///     the same key and text.
///   - An entry of `old` with a translation, whose key is not in the template, moves to a new key
///     of the template that has the same scope and the same position in it. The new entry is
///     `fuzzy`.
///   - Any other such entry becomes obsolete (`#~`). Obsolete entries stay in the file, and come
///     back when their key is extracted again.
/// The header of `old` stays (a new file has a new one). The entries come in the order of the
/// template, then the obsolete ones.
[[nodiscard]] PoFile update_translation(const PoFile& tmpl, const PoFile& old,
                                        std::string_view language);

/// The header entry of a new file. `language` is empty for the template.
[[nodiscard]] PoEntry po_header(std::string_view language);

}  // namespace vnmaki
