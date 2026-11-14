#include "cli/new_command.hpp"

#include <cstddef>
#include <filesystem>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/utf8_path.hpp"
#include "engine/platform/sdl_platform.hpp"
#include "engine/runtime/new_project.hpp"

namespace vnmaki::cli {

namespace {

constexpr int kExitError = 1;
constexpr int kExitUsage = 2;
constexpr std::string_view kTemplateName = "default";
constexpr std::string_view kLangOption = "--lang";

}  // namespace

std::vector<std::string> available_addons() {
  return list_addons(executable_directory(), VNMAKI_SOURCE_TEMPLATES_DIR);
}

int new_command(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  // <name> and any number of `--lang <code>`, in any order.
  std::vector<std::string> languages;
  std::string_view name;
  bool usage_ok = !args.empty();
  for (std::size_t i = 0; usage_ok && i < args.size(); ++i) {
    if (args[i] == kLangOption && i + 1 < args.size()) {
      languages.emplace_back(args[++i]);
    } else if (name.empty() && !args[i].starts_with("-")) {
      name = args[i];
    } else {
      usage_ok = false;
    }
  }
  if (!usage_ok || name.empty()) {
    err << "usage:" << kNewUsage.substr(std::string_view{"      "}.size());
    return kExitUsage;
  }
  const std::filesystem::path target = utf8_path(name);
  try {
    const std::filesystem::path templates =
        find_template(kTemplateName, executable_directory(), VNMAKI_SOURCE_TEMPLATES_DIR)
            .value_or(std::filesystem::path{});
    if (templates.empty()) {
      err << "vnmaki: cannot find the template '" << kTemplateName << "'\n";
      return kExitError;
    }
    // Every add-on is found before the folder is made, so an unknown one writes nothing.
    std::vector<std::filesystem::path> addons;
    for (const std::string& language : languages) {
      const auto addon = find_addon(language, executable_directory(), VNMAKI_SOURCE_TEMPLATES_DIR);
      if (!addon) {
        err << "vnmaki: unknown language add-on '" << language << "'; available:";
        for (const std::string& known : available_addons()) {
          err << ' ' << known;
        }
        err << '\n';
        return kExitError;
      }
      addons.push_back(*addon);
    }
    const std::string title = project_title(target);
    create_project(templates, target, title);
    for (const std::filesystem::path& addon : addons) {
      apply_addon(addon, target);
    }
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
