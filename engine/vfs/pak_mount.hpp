#pragma once

// A `.vnpak` archive as a VfsMount (docs/SPEC.md section 19). The file form (integers are
// little-endian):
//
//   header   "VNPK", u32 version (kPakVersion)
//   data     the zstd frame of each file, one after the other
//   index    u32 entry count, then each entry, sorted by path (byte order, no duplicates):
//            u32 path length, the path (UTF-8, canonical, see path.hpp), u64 offset of the frame
//            (from the start of the file), u64 frame size, u64 raw size, u64 hash of the raw bytes
//            (hash_bytes, engine/assets/import_cache.hpp)
//   footer   u64 offset of the index, "VNPE"
//
// There are no directory entries: a directory exists when a path goes through it.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "engine/vfs/vfs.hpp"

namespace vnmaki {

/// A pak that cannot be read, or a file that cannot be added to one. The message says why.
class PakError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

inline constexpr std::string_view kPakMagic = "VNPK";
inline constexpr std::string_view kPakFooterMagic = "VNPE";
inline constexpr std::uint32_t kPakVersion = 1;
inline constexpr std::size_t kPakHeaderSize = 8;
inline constexpr std::size_t kPakFooterSize = 12;
/// The most a file can have raw. The reader checks it before it allocates.
inline constexpr std::uint64_t kPakMaxFileSize = std::uint64_t{1} << 30;

inline constexpr std::string_view kGamePakName = "game.vnpak";
inline constexpr std::string_view kPatchPakPrefix = "patch-";
inline constexpr std::string_view kPakExtension = ".vnpak";

class PakMount final : public VfsMount {
 public:
  /// Reads the index of the pak file. The files are read from it when they are used, so the file
  /// must stay. Throws PakError if it cannot be opened or its structure is wrong (bad magic or
  /// version, a path that is not canonical, an unsorted index, a frame outside the file).
  [[nodiscard]] static std::unique_ptr<PakMount> open(const std::filesystem::path& file);

  /// The same for a pak in memory.
  [[nodiscard]] static std::unique_ptr<PakMount> from_bytes(std::vector<std::byte> bytes);

  [[nodiscard]] bool is_file(std::string_view path) const override;
  /// Nothing for a file that is missing or damaged (use read_file for the reason).
  [[nodiscard]] std::optional<std::vector<std::byte>> read(std::string_view path) const override;
  [[nodiscard]] std::optional<std::vector<VfsEntry>> list(
      std::string_view directory) const override;

  /// The raw bytes of a file. Throws PakError if there is no such file, the frame is damaged or
  /// the hash does not match. Const methods are thread-safe.
  [[nodiscard]] std::vector<std::byte> read_file(std::string_view path) const;

  /// Every path, sorted.
  [[nodiscard]] std::vector<std::string> paths() const;

 private:
  struct Entry {
    std::string path{};
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint64_t raw_size = 0;
    std::uint64_t hash = 0;
  };

  PakMount() = default;
  void parse_index(std::uint64_t file_size);
  [[nodiscard]] std::vector<std::byte> read_range(std::uint64_t offset, std::uint64_t size) const;
  [[nodiscard]] const Entry* find(std::string_view path) const;

  std::filesystem::path file_{};     ///< Empty for a pak in memory.
  std::vector<std::byte> memory_{};  ///< The pak, when `file_` is empty.
  std::vector<Entry> entries_{};     ///< Sorted by path.
  std::map<std::string, std::vector<VfsEntry>, std::less<>> children_{};  ///< By directory.
};

/// Mounts `<directory>/game.vnpak` on `vfs`, then every `patch-*.vnpak` of the folder in name
/// order, so a patch overrides the game and a later patch overrides an earlier one. Returns false
/// (and mounts nothing) when there is no `game.vnpak`. Throws PakError for a pak that cannot be
/// read; the mounts before it stay.
bool mount_game_packs(Vfs& vfs, const std::filesystem::path& directory);

}  // namespace vnmaki
