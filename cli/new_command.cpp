#include "cli/new_command.hpp"

#include <filesystem>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

#include "engine/core/utf8_path.hpp"
#include "engine/platform/sdl_platform.hpp"
#include "engine/runtime/new_project.hpp"

namespace vnmaki::cli {

namespace {

constexpr int kExitError = 1;
constexpr int kExitUsage = 2;
constexpr std::string_view kTemplateName = "default";

}  // namespace

int new_command(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  if (args.size() != 1 || args.front().starts_with("-")) {
    err << "usage:" << kNewUsage.substr(std::string_view{"      "}.size());
    return kExitUsage;
  }
  const std::filesystem::path target = utf8_path(args.front());
  try {
    const std::filesystem::path templates =
        find_template(kTemplateName, executable_directory(), VNMAKI_SOURCE_TEMPLATES_DIR)
            .value_or(std::filesystem::path{});
    if (templates.empty()) {
      err << "vnmaki: cannot find the template '" << kTemplateName << "'\n";
      return kExitError;
    }
    const std::string title = project_title(target);
    create_project(templates, target, title);
    out << "Created \"" << title << "\" in \"" << to_utf8(target)
        << "\"\nPlay it with: vnmaki run \"" << to_utf8(target) << "\"\n";
  } catch (const NewProjectError& error) {
    err << "vnmaki: " << error.what() << '\n';
    return kExitError;
  } catch (const PlatformError& error) {
    err << "vnmaki: " << error.what() << '\n';
    return kExitError;
  }
  return 0;
}

}  // namespace vnmaki::cli
