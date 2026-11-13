#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace vnmaki::cli {

inline constexpr std::string_view kExtractUsage = "       vnmaki extract-strings <dir>\n";

/// `vnmaki extract-strings <dir>` (docs/SPEC.md section 16, engine/l10n/extract.hpp): writes
/// `translations/template.pot` of the project <dir> and updates `translations/<lang>.po` for each
/// language of `project.toml` after the first (the source language). It prints one line for each
/// file written. It writes nothing when a `.po` file cannot be read.
///
/// `args` are the arguments after "extract-strings". Returns the process exit code: 0 on success,
/// 1 when <dir> is not a folder, `project.toml` is not valid, a `.po` file cannot be read or a file
/// cannot be written, 2 on a usage error.
int extract_command(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace vnmaki::cli
