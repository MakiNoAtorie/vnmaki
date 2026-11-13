#include "cli/check_command.hpp"

#include <filesystem>
#include <ostream>
#include <span>
#include <string_view>
#include <vector>

#include "engine/tools/check.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki::cli {

namespace {

constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

}  // namespace

int check_command(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  if (args.size() != 1 || args.front().starts_with("-")) {
    err << "usage:" << kCheckUsage.substr(std::string_view{"      "}.size());
    return kExitUsage;
  }
  const std::filesystem::path directory{args.front()};
  Vfs vfs;
  if (!vfs.mount(make_directory_mount(directory))) {
    err << "vnmaki: " << directory << " is not a directory\n";
    return kExitError;
  }
  const std::vector<Finding> findings = check_project(vfs);
  out << findings_to_csv(findings);
  return has_errors(findings) ? kExitError : 0;
}

}  // namespace vnmaki::cli
