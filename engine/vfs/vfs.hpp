#pragma once

// The engine's virtual file system. Every file that game code can reach is
// read through a Vfs by virtual path (see engine/vfs/path.hpp for the path
// rules and the case policy), never with fopen or std::ifstream directly
// (docs/SPEC.md section 3).

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vnmaki {

/// One entry of a directory listing.
struct VfsEntry {
  std::string name;  ///< A single path segment, e.g. "bg.png".
  bool is_directory = false;

  friend bool operator==(const VfsEntry&, const VfsEntry&) = default;
};

/// A source of files for the Vfs: a loose-file directory in dev mode, later
/// a .vnpak archive. Every path a mount receives is already in canonical form
/// (normalize_virtual_path), relative to the mount's own root, with "" being
/// the root.
class VfsMount {
 public:
  VfsMount() = default;
  VfsMount(const VfsMount&) = delete;
  VfsMount(VfsMount&&) = delete;
  VfsMount& operator=(const VfsMount&) = delete;
  VfsMount& operator=(VfsMount&&) = delete;
  virtual ~VfsMount() = default;

  /// Whether `path` names a file (not a directory) in this mount.
  [[nodiscard]] virtual bool is_file(std::string_view path) const = 0;

  /// The whole content of the file at `path`, or nothing if it is not a file
  /// here or cannot be read.
  [[nodiscard]] virtual std::optional<std::vector<std::byte>> read(std::string_view path) const = 0;

  /// The entries of directory `path` in any order, or nothing if it is not a
  /// directory here.
  [[nodiscard]] virtual std::optional<std::vector<VfsEntry>> list(
      std::string_view directory) const = 0;
};

/// A mount serving the loose files under host directory `root` (dev mode).
/// Returns nullptr if `root` is not an existing directory.
///
/// Lookups match every path segment by its exact spelling, even on a
/// case-insensitive host file system. Host names that are not valid virtual
/// path segments (see normalize_virtual_path) are unreachable and not listed.
/// Symbolic links inside the root are followed.
[[nodiscard]] std::unique_ptr<VfsMount> make_directory_mount(const std::filesystem::path& root);

/// An ordered stack of mounts searched from the most recently mounted down.
/// A later mount overrides an earlier one by path, so a patch mounted on top
/// of the base game replaces just the files it contains.
///
/// Paths passed in are normalized first; a path normalize_virtual_path
/// rejects finds nothing.
///
/// Thread safety: the const members may be called from any number of threads
/// at once. mount() must not run concurrently with anything else.
class Vfs {
 public:
  /// Puts `mount` on top of the stack. Returns false, and changes nothing, if
  /// `mount` is null.
  bool mount(std::unique_ptr<VfsMount> mount);

  /// Whether `path` names a file (not a directory) in any mount.
  [[nodiscard]] bool exists(std::string_view path) const;

  /// The whole content of the file at `path`, read from the topmost mount
  /// that has it, or nothing if no mount does or that read fails. A failed
  /// read never falls back to a lower mount's older copy.
  [[nodiscard]] std::optional<std::vector<std::byte>> read(std::string_view path) const;

  /// The merged entries of directory `directory` across all mounts, sorted
  /// by name (byte order), each name once. Where mounts disagree on whether a
  /// name is a file or a directory, the topmost wins. A missing directory, a
  /// file or a rejected path gives an empty list.
  [[nodiscard]] std::vector<VfsEntry> list(std::string_view directory) const;

 private:
  [[nodiscard]] const VfsMount* find_file(std::string_view path) const;

  std::vector<std::unique_ptr<VfsMount>> mounts_;
};

}  // namespace vnmaki
