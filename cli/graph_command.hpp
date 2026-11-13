#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace vnmaki::cli {

inline constexpr std::string_view kGraphUsage = "       vnmaki graph <dir> [--format dot|csv]\n";

/// `vnmaki graph <dir> [--format dot|csv]` (docs/SPEC.md section 18): scans the scripts of the
/// project <dir> and prints the story graph to `out`, as Graphviz DOT (the default) or CSV. It does
/// not run the story. See engine/tools/story_graph.hpp for what the output says.
///
/// `args` are the arguments after "graph". Returns the process exit code: 0, 1 when <dir> is not a
/// folder, 2 on a usage error.
int graph_command(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace vnmaki::cli
