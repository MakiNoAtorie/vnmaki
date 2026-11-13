#pragma once

// Dev-mode import (docs/SPEC.md sections 18 and 19): the files that the file watcher reports go
// through the importers of engine/assets/, so the import cache is ready when the game is built.
// The game itself keeps loading the loose files.

#include <filesystem>
#include <span>
#include <string>

#include "engine/assets/importer.hpp"
#include "engine/tools/log_buffer.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki {

class HotImport {
 public:
  /// `vfs` serves `project_dir`; both and `log` must outlive this.
  HotImport(const Vfs& vfs, const std::filesystem::path& project_dir, LogBuffer& log)
      : importer_(vfs, project_dir), log_(&log) {}

  /// Imports one batch of changed paths (as FileWatcher::poll gives them). A new import is an info
  /// entry in the log, a failure an error entry; a cache hit is silent. Shaders are left to
  /// HotReload, which compiles them and reports their errors. It runs in the calling frame.
  // ponytail: synchronous, so a long audio file stalls one frame; run it as a job if that hurts.
  void apply(std::span<const std::string> paths);

 private:
  Importer importer_;
  LogBuffer* log_;
};

}  // namespace vnmaki
