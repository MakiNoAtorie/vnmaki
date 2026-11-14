#include "engine/vfs/vfs.hpp"

#include <exception>
#include <fstream>
#include <functional>
#include <map>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>

#include "engine/vfs/path.hpp"

namespace vnmaki {
namespace {

namespace fs = std::filesystem;

// Virtual paths are UTF-8; std::filesystem only reads a narrow string as
// UTF-8 when it comes in as char8_t.
fs::path host_path_of(std::string_view utf8) {
  std::u8string u8;
  u8.reserve(utf8.size());
  for (const char c : utf8) {
    u8.push_back(static_cast<char8_t>(c));
  }
  return fs::path{u8};
}

// The file name of `host_path` as UTF-8, or nothing if the host name has no
// UTF-8 form (an unpaired surrogate in a Windows name makes u8string() throw).
std::optional<std::string> utf8_name_of(const fs::path& host_path) {
  std::u8string u8;
  try {
    u8 = host_path.filename().u8string();
  } catch (const std::exception&) {
    return std::nullopt;
  }
  std::string utf8;
  utf8.reserve(u8.size());
  for (const char8_t c : u8) {
    utf8.push_back(static_cast<char>(c));
  }
  return utf8;
}

// Whether `directory` holds an entry spelled exactly `name`. A plain stat
// cannot tell: case-insensitive file systems would also find "NAME".
bool has_entry_spelled(const fs::path& directory, std::string_view name) {
  std::error_code error;
  for (fs::directory_iterator it{directory, error}; !error && it != fs::directory_iterator{};
       it.increment(error)) {
    if (utf8_name_of(it->path()) == name) {
      return true;
    }
  }
  return false;
}

std::optional<std::vector<std::byte>> read_host_file(const fs::path& file) {
  std::error_code error;
  if (!fs::is_regular_file(file, error)) {
    return std::nullopt;
  }
  const std::uintmax_t size = fs::file_size(file, error);
  if (error) {
    return std::nullopt;
  }
  std::ifstream stream{file, std::ios::binary};
  if (!stream) {
    return std::nullopt;
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  // std::byte has no stream support; reading through char* is the sanctioned
  // way to fill raw storage (char may alias any object).
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
    return std::nullopt;
  }
  return bytes;
}

// Serves a host directory's loose files. Every lookup walks the path one
// segment at a time and checks each segment's exact spelling, which keeps
// virtual paths case-sensitive on every host. That costs a directory scan per
// segment; fine for dev mode, and release builds read packs instead.
class DirectoryMount final : public VfsMount {
 public:
  explicit DirectoryMount(fs::path root) : root_{std::move(root)} {}

  [[nodiscard]] bool is_file(std::string_view path) const override {
    const std::optional<fs::path> host = resolve(path);
    std::error_code error;
    return host && fs::is_regular_file(*host, error);
  }

  [[nodiscard]] std::optional<std::vector<std::byte>> read(std::string_view path) const override {
    const std::optional<fs::path> host = resolve(path);
    if (!host) {
      return std::nullopt;
    }
    return read_host_file(*host);
  }

  [[nodiscard]] std::optional<std::vector<VfsEntry>> list(
      std::string_view directory) const override {
    const std::optional<fs::path> host = resolve(directory);
    std::error_code error;
    if (!host || !fs::is_directory(*host, error)) {
      return std::nullopt;
    }
    std::vector<VfsEntry> entries;
    for (fs::directory_iterator it{*host, error}; !error && it != fs::directory_iterator{};
         it.increment(error)) {
      std::optional<std::string> name = utf8_name_of(it->path());
      // A name that is not a canonical one-segment virtual path could never
      // be read through the VFS, so it is not listed either.
      if (!name || normalize_virtual_path(*name) != name) {
        continue;
      }
      std::error_code kind_error;
      if (it->is_directory(kind_error)) {
        entries.push_back({.name = std::move(*name), .is_directory = true});
      } else if (it->is_regular_file(kind_error)) {
        entries.push_back({.name = std::move(*name), .is_directory = false});
      }
    }
    return entries;
  }

 private:
  // The host path of a normalized virtual path, if every segment exists with
  // exactly that spelling.
  [[nodiscard]] std::optional<fs::path> resolve(std::string_view path) const {
    fs::path host = root_;
    while (!path.empty()) {
      const std::size_t slash = path.find('/');
      const std::string_view segment = path.substr(0, slash);
      fs::path next = host / host_path_of(segment);
      std::error_code error;
      // The stat rejects most misses without scanning the directory.
      if (!fs::exists(next, error) || !has_entry_spelled(host, segment)) {
        return std::nullopt;
      }
      host = std::move(next);
      path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
    }
    return host;
  }

  fs::path root_;
};

}  // namespace

std::unique_ptr<VfsMount> make_directory_mount(const std::filesystem::path& root) {
  std::error_code error;
  if (!fs::is_directory(root, error)) {
    return nullptr;
  }
  // Pin the root now, so a later change of working directory cannot move it.
  fs::path absolute_root = fs::absolute(root, error);
  if (error) {
    return nullptr;
  }
  return std::make_unique<DirectoryMount>(std::move(absolute_root));
}

bool Vfs::mount(std::unique_ptr<VfsMount> mount) {
  if (!mount) {
    return false;
  }
  mounts_.push_back(std::move(mount));
  return true;
}

bool Vfs::exists(std::string_view path) const {
  const std::optional<std::string> normalized = normalize_virtual_path(path);
  return normalized && find_file(*normalized) != nullptr;
}

std::optional<std::vector<std::byte>> Vfs::read(std::string_view path) const {
  const std::optional<std::string> normalized = normalize_virtual_path(path);
  if (!normalized) {
    return std::nullopt;
  }
  const VfsMount* mount = find_file(*normalized);
  if (mount == nullptr) {
    return std::nullopt;
  }
  return mount->read(*normalized);
}

std::vector<VfsEntry> Vfs::list(std::string_view directory) const {
  const std::optional<std::string> normalized = normalize_virtual_path(directory);
  if (!normalized) {
    return {};
  }
  // Keyed by name, so the result comes out sorted and the first (topmost)
  // mount to report a name decides whether it is a file or a directory.
  std::map<std::string, bool, std::less<>> merged;
  for (const auto& mount : std::views::reverse(mounts_)) {
    if (const auto entries = mount->list(*normalized)) {
      for (const VfsEntry& entry : *entries) {
        merged.try_emplace(entry.name, entry.is_directory);
      }
    }
  }
  std::vector<VfsEntry> result;
  result.reserve(merged.size());
  for (const auto& [name, is_directory] : merged) {
    result.push_back({.name = name, .is_directory = is_directory});
  }
  return result;
}

const VfsMount* Vfs::find_file(std::string_view path) const {
  for (const auto& mount : std::views::reverse(mounts_)) {
    if (mount->is_file(path)) {
      return mount.get();
    }
  }
  return nullptr;
}

}  // namespace vnmaki
