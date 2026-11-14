#pragma once

// The import cache (docs/SPEC.md section 19): the output of an importer, in `.vnmaki/cache/` of the
// project. The key is a hash of the source file, the importer's name and version, and the settings
// that change its output (`params`). A cache hit needs no import.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vnmaki {

/// The cache folder of a project, from the project folder.
inline constexpr std::string_view kImportCacheDir = ".vnmaki/cache";

/// FNV-1a over 64 bits. Not secure; the cache only needs to tell changed files apart.
[[nodiscard]] std::uint64_t hash_bytes(std::span<const std::byte> bytes);

/// "<importer>-<16 hex digits>": the digits are `hash_bytes` of the source, the importer name, its
/// version and `params`, each followed by a NUL byte.
[[nodiscard]] std::string import_cache_key(std::span<const std::byte> source,
                                           std::string_view importer, std::uint32_t version,
                                           std::string_view params = {});

class ImportCache {
 public:
  /// The cache in the folder `directory` (made when the first entry is stored).
  explicit ImportCache(std::filesystem::path directory) : directory_(std::move(directory)) {}

  /// The cache of the project folder `project`.
  [[nodiscard]] static ImportCache for_project(const std::filesystem::path& project) {
    return ImportCache{project / std::filesystem::path{kImportCacheDir}};
  }

  /// The stored output, or nothing for a key that was not stored.
  [[nodiscard]] std::optional<std::vector<std::byte>> find(const std::string& key) const;

  /// Stores `data` under `key`. It writes `<key>.tmp` and renames it, so a failed write leaves no
  /// half entry. Throws std::runtime_error if it cannot write.
  void store(const std::string& key, std::span<const std::byte> data) const;

  [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }

 private:
  std::filesystem::path directory_;
};

}  // namespace vnmaki
