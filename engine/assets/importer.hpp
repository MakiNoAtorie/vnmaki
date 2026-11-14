#pragma once

// The asset import of a project (docs/SPEC.md section 19): the importers run over the files of the
// project and keep their output in the import cache (import_cache.hpp).
//
//   source                              importer     output (virtual path)
//   `X.wav`, `X.flac`                   audio        `X.opus`
//   `X.ttf`, `X.otf`                    font-subset  `X.<language>.ttf` for each language
//   `shaders/X.frag.hlsl`               shader       `shaders/X.frag.vnshader`
//   `X.gltf`, `X.glb`                   gltf         (a check only: the file is used as it is)
//
// A file of the project that has no importer is used as it is.

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/import_cache.hpp"
#include "engine/assets/import_settings.hpp"
#include "engine/runtime/project_config.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki {

struct ImportedFile {
  std::string source{};  ///< The virtual path of the file that was imported.
  /// The virtual path of the output; empty for a file that was only checked (glTF) or kept.
  std::string path{};
  std::vector<std::byte> data{};  ///< The output.
  bool cached = false;            ///< The cache had it, so no importer ran.
  bool kept = false;              ///< A font of `[build.fonts] keep`: used as it is, no subset.
};

struct ImportReport {
  std::vector<ImportedFile> files{};
  /// One message for each file that failed, "<path>: <problem>". The other files are still
  /// imported.
  std::vector<std::string> problems{};
};

class Importer {
 public:
  /// Imports the project in `project_dir`, which `vfs` serves (`vfs` must outlive this). The cache
  /// is `project_dir/.vnmaki/cache`. The two functions below read project.toml each time. They
  /// throw ProjectConfigError and ImportSettingsError for a bad one, and std::runtime_error if the
  /// cache cannot be written.
  Importer(const Vfs& vfs, std::filesystem::path project_dir);

  /// Imports every file that has an importer, in the order of the virtual paths.
  [[nodiscard]] ImportReport import_all() const;

  /// Imports the changed files of a batch of the file watcher (paths from the project folder, `/`
  /// separators). A changed script, `.po` file or `project.toml` imports the fonts again, because
  /// the characters of the languages may have changed; the cache makes that cheap. Files that are
  /// gone, have no importer or are in `.vnmaki/` do nothing.
  [[nodiscard]] ImportReport import_changed(std::span<const std::string> paths) const;

 private:
  [[nodiscard]] ImportReport import_paths(const std::vector<std::string>& sources) const;

  const Vfs* vfs_;
  ImportCache cache_;
};

/// Writes the output of `report` under `directory`, at the virtual paths. Throws
/// std::runtime_error if a file cannot be written.
void write_imported_files(const ImportReport& report, const std::filesystem::path& directory);

}  // namespace vnmaki
