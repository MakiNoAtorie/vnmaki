#pragma once

// Writes a `.vnpak` archive (the format is in pak_mount.hpp).

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vnmaki {

class PakWriter {
 public:
  /// Writes the header to `out`, which must outlive the writer. Throws PakError if that fails.
  explicit PakWriter(std::ostream& out);

  /// Compresses `data` with zstd and writes it. `path` must be a canonical virtual path
  /// (normalize_virtual_path), not yet added, and must not be a folder or inside a file of the
  /// pak. Throws PakError otherwise, and for a file over kPakMaxFileSize or a failed write.
  void add(std::string_view path, std::span<const std::byte> data);

  /// Writes the index and the footer. Call it once, last. Throws PakError if a write fails.
  void finish();

 private:
  struct Entry {
    std::string path{};
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint64_t raw_size = 0;
    std::uint64_t hash = 0;
  };

  void write(std::span<const std::byte> bytes);

  std::ostream* out_;
  std::uint64_t position_ = 0;
  bool finished_ = false;
  std::vector<Entry> entries_{};
  std::set<std::string> paths_{};
};

}  // namespace vnmaki
