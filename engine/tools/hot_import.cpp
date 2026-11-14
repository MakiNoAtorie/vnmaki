#include "engine/tools/hot_import.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace vnmaki {

void HotImport::apply(std::span<const std::string> paths) {
  std::vector<std::string> changed;
  std::ranges::copy_if(paths, std::back_inserter(changed),
                       [](const std::string& path) { return !path.ends_with(".hlsl"); });
  if (changed.empty()) {
    return;
  }
  try {
    const ImportReport report = importer_.import_changed(changed);
    for (const ImportedFile& file : report.files) {
      if (!file.path.empty() && !file.cached) {
        log_->add(LogLevel::kInfo, "imported " + file.source + " -> " + file.path);
      }
    }
    for (const std::string& problem : report.problems) {
      log_->add(LogLevel::kError, "import: " + problem);
    }
  } catch (const std::runtime_error& error) {
    // A bad project.toml: the scripts reload shows that error too, so the log gets it once more.
    log_->add(LogLevel::kError, std::string{"import: "} + error.what());
  }
}

}  // namespace vnmaki
