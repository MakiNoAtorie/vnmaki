#pragma once

#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vnmaki::cli {

inline constexpr std::string_view kNewUsage = "       vnmaki new <name> [--lang <code>]...\n";

/// The names of the language add-ons (`templates/addons/*`) that `--lang` takes.
[[nodiscard]] std::vector<std::string> available_addons();

/// `vnmaki new <name> [--lang <code>]...` (docs/SPEC.md section 19): makes the folder <name> from
/// the template `default` and sets the game's title to the last part of <name>. Each `--lang` adds
/// the language add-on of that code (`apply_addon`); an unknown code stops the command before it
/// writes anything. The folder must not exist or must be empty. The template is
/// `<folder of vnmaki>/templates/default`, or else `templates/default` of the source tree that
/// vnmaki was built from; the add-ons are in `templates/addons/` next to it.
///
/// `args` are the arguments after "new". Returns the process exit code: 0, 1 when the folder is
/// not empty or no template or add-on is found, 2 on a usage error.
int new_command(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace vnmaki::cli
