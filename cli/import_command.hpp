#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace vnmaki::cli {

inline constexpr std::string_view kImportUsage = "       vnmaki import <dir> [--out <folder>]\n";

/// `vnmaki import <dir> [--out <folder>]` (docs/SPEC.md section 19, engine/assets/importer.hpp):
/// imports the audio, fonts, shaders and glTF files of the project <dir> into the cache
/// `<dir>/.vnmaki/cache`. It prints one line for each file: `<source> -> <output> (imported)`,
/// `... (cached)` when the cache had it, `<source> (valid)` for a glTF file, or `<source> (kept)`
/// for a font of `[build.fonts] keep`. With `--out` it
/// also writes the outputs to <folder> at their virtual paths (the files that `vnmaki build` packs
/// in place of the sources). A file that fails does not stop the others; its message goes to `err`.
///
/// `args` are the arguments after "import". Returns the process exit code: 0 when every file was
/// imported, 1 when <dir> is not a project or a file failed, 2 on a usage error.
int import_command(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace vnmaki::cli
