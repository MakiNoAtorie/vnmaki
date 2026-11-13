#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace vnmaki::cli {

inline constexpr std::string_view kCheckUsage = "       vnmaki check <dir>\n";

/// `vnmaki check <dir>` (docs/SPEC.md sections 18 and 19): lints the project <dir> without running
/// it and prints the findings to `out` as CSV `file,line,severity,code,message`. See
/// engine/tools/check.hpp for the codes.
///
/// `args` are the arguments after "check". Returns the process exit code: 0 when there is no
/// error (warnings are fine), 1 when there is an error or <dir> is not a folder, 2 on a usage
/// error.
int check_command(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace vnmaki::cli
