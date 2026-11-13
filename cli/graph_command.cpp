#include "cli/graph_command.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <ostream>
#include <span>
#include <string_view>

#include "engine/tools/story_graph.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki::cli {

namespace {

constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

}  // namespace

int graph_command(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  std::optional<std::filesystem::path> directory;
  bool csv = false;
  bool usage_error = false;
  for (std::size_t i = 0; i < args.size() && !usage_error; ++i) {
    if (args[i] == "--format" && i + 1 < args.size() &&
        (args[i + 1] == "dot" || args[i + 1] == "csv")) {
      csv = args[i + 1] == "csv";
      ++i;
    } else if (args[i].starts_with("-") || directory) {
      usage_error = true;
    } else {
      directory = std::filesystem::path{args[i]};
    }
  }
  if (usage_error || !directory) {
    err << "usage:" << kGraphUsage.substr(std::string_view{"      "}.size());
    return kExitUsage;
  }

  Vfs vfs;
  if (!vfs.mount(make_directory_mount(*directory))) {
    err << "vnmaki: " << *directory << " is not a directory\n";
    return kExitError;
  }
  const StoryGraph graph = build_story_graph(read_scripts(vfs));
  out << (csv ? to_csv(graph) : to_dot(graph));
  return 0;
}

}  // namespace vnmaki::cli
