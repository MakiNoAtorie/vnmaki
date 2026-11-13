#include "cli/import_command.hpp"

#include <cstddef>
#include <filesystem>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "engine/assets/importer.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki::cli {

namespace {

constexpr int kExitError = 1;
constexpr int kExitUsage = 2;
constexpr std::string_view kOutOption = "--out";

}  // namespace

int import_command(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  std::string_view directory;
  std::string_view output;
  bool usage_error = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == kOutOption && i + 1 < args.size() && output.empty()) {
      output = args[++i];
    } else if (!args[i].starts_with("-") && directory.empty()) {
      directory = args[i];
    } else {
      usage_error = true;
    }
  }
  if (usage_error || directory.empty()) {
    err << "usage:" << kImportUsage.substr(std::string_view{"      "}.size());
    return kExitUsage;
  }

  const std::filesystem::path project{directory};
  Vfs vfs;
  if (!vfs.mount(make_directory_mount(project))) {
    err << "vnmaki: " << project << " is not a directory\n";
    return kExitError;
  }
  try {
    const Importer importer{vfs, project};
    const ImportReport report = importer.import_all();
    for (const ImportedFile& file : report.files) {
      out << file.source;
      if (file.path.empty()) {
        out << " (valid)";
      } else {
        out << " -> " << file.path << (file.cached ? " (cached)" : " (imported)");
      }
      out << "\n";
    }
    if (!output.empty()) {
      write_imported_files(report, std::filesystem::path{output});
    }
    for (const std::string& problem : report.problems) {
      err << "vnmaki: " << problem << "\n";
    }
    return report.problems.empty() ? 0 : kExitError;
  } catch (const ProjectConfigError& error) {
    err << "vnmaki: " << error.what() << "\n";
  } catch (const ImportSettingsError& error) {
    err << "vnmaki: " << error.what() << "\n";
  } catch (const std::runtime_error& error) {
    err << "vnmaki: " << error.what() << "\n";
  }
  return kExitError;
}

}  // namespace vnmaki::cli
