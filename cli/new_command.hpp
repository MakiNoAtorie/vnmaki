#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace vnmaki::cli {

inline constexpr std::string_view kNewUsage = "       vnmaki new <name>\n";

/// `vnmaki new <name>` (docs/SPEC.md section 19): makes the folder <name> from the template
/// `default` and sets the game's title to the last part of <name>. The folder must not exist or
/// must be empty. The template is `<folder of vnmaki>/templates/default`, or else `templates/default`
/// of the source tree that vnmaki was built from.
///
/// `args` are the arguments after "new". Returns the process exit code: 0, 1 when the folder is
/// not empty or no template is found, 2 on a usage error.
int new_command(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace vnmaki::cli
