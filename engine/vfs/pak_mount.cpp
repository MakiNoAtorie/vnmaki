#include "engine/vfs/pak_mount.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <new>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <zstd.h>

#include "engine/assets/import_cache.hpp"
#include "engine/core/byte_stream.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/vfs/path.hpp"

namespace vnmaki {

namespace {

namespace fs = std::filesystem;

// A path length (u32) and the four u64 fields, with an empty path: the least an entry takes.
constexpr std::size_t kMinEntrySize = sizeof(std::uint32_t) + 4 * sizeof(std::uint64_t);
constexpr std::size_t kMagicSize = 4;

bool has_text(std::span<const std::byte> bytes, std::string_view text) {
  return bytes.size() >= text.size() && std::memcmp(bytes.data(), text.data(), text.size()) == 0;
}

}  // namespace

std::unique_ptr<PakMount> PakMount::open(const fs::path& file) {
  std::error_code error;
  const std::uintmax_t size = fs::file_size(file, error);
  if (error) {
    throw PakError{"pak: cannot open " + to_utf8(file)};
  }
  std::unique_ptr<PakMount> pak{new PakMount};
  pak->file_ = file;
  try {
    pak->parse_index(size);
  } catch (const PakError& failure) {
    throw PakError{to_utf8(file) + ": " + failure.what()};
  }
  return pak;
}

std::unique_ptr<PakMount> PakMount::from_bytes(std::vector<std::byte> bytes) {
  std::unique_ptr<PakMount> pak{new PakMount};
  pak->memory_ = std::move(bytes);
  pak->parse_index(pak->memory_.size());
  return pak;
}

std::vector<std::byte> PakMount::read_range(std::uint64_t offset, std::uint64_t size) const {
  if (file_.empty()) {
    if (offset > memory_.size() || size > memory_.size() - offset) {
      throw PakError{"data outside the file"};
    }
    const auto first = memory_.begin() + static_cast<std::ptrdiff_t>(offset);
    return {first, first + static_cast<std::ptrdiff_t>(size)};
  }
  std::ifstream stream{file_, std::ios::binary};
  stream.seekg(static_cast<std::streamoff>(offset));
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  // std::byte has no stream support; char* may alias any object.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
  if (stream.gcount() != static_cast<std::streamsize>(size)) {
    throw PakError{"cannot read the file"};
  }
  return bytes;
}

void PakMount::parse_index(std::uint64_t file_size) {
  if (file_size < kPakHeaderSize + kPakFooterSize) {
    throw PakError{"pak: too short"};
  }
  const std::vector<std::byte> header = read_range(0, kPakHeaderSize);
  if (!has_text(header, kPakMagic)) {
    throw PakError{"pak: not a vnpak file"};
  }
  ByteReader version{std::span{header}.subspan(kMagicSize), "pak"};
  if (version.u32() != kPakVersion) {
    throw PakError{"pak: unsupported version"};
  }
  const std::vector<std::byte> footer = read_range(file_size - kPakFooterSize, kPakFooterSize);
  if (!has_text(std::span{footer}.subspan(sizeof(std::uint64_t)), kPakFooterMagic)) {
    throw PakError{"pak: the end of the file is damaged"};
  }
  const std::uint64_t index_offset = ByteReader{footer, "pak"}.u64();
  const std::uint64_t index_end = file_size - kPakFooterSize;
  if (index_offset < kPakHeaderSize || index_offset > index_end ||
      index_end - index_offset < sizeof(std::uint32_t)) {
    throw PakError{"pak: bad index offset"};
  }
  const std::vector<std::byte> index = read_range(index_offset, index_end - index_offset);

  try {
    ByteReader reader{index, "pak index"};
    const std::uint32_t count = reader.u32();
    if (count > (index.size() - sizeof(std::uint32_t)) / kMinEntrySize) {
      throw PakError{"pak: bad entry count"};
    }
    entries_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      Entry entry;
      entry.path = reader.string();
      entry.offset = reader.u64();
      entry.size = reader.u64();
      entry.raw_size = reader.u64();
      entry.hash = reader.u64();
      if (entry.path.empty() || normalize_virtual_path(entry.path) != entry.path) {
        throw PakError{"pak: a path is not a canonical virtual path"};
      }
      if (!entries_.empty() && entries_.back().path >= entry.path) {
        throw PakError{"pak: the index is not sorted"};
      }
      if (entry.offset < kPakHeaderSize || entry.offset > index_offset ||
          entry.size > index_offset - entry.offset) {
        throw PakError{"pak: " + entry.path + " is outside the data"};
      }
      if (entry.raw_size > kPakMaxFileSize) {
        throw PakError{"pak: " + entry.path + " is too large"};
      }
      entries_.push_back(std::move(entry));
    }
    if (!reader.at_end()) {
      throw PakError{"pak: bytes after the index"};
    }
  } catch (const std::invalid_argument& failure) {
    throw PakError{failure.what()};
  }

  // The folders. The sort puts "a" before "a/b", so a folder can only clash with a file that the
  // loop has seen.
  children_[""];
  for (const Entry& entry : entries_) {
    std::string folder;
    std::string_view rest = entry.path;
    while (true) {
      const std::size_t slash = rest.find('/');
      const std::string_view name = rest.substr(0, slash);
      const std::string path =
          folder.empty() ? std::string{name} : folder + "/" + std::string{name};
      if (slash == std::string_view::npos) {
        if (children_.contains(path)) {
          throw PakError{"pak: " + path + " is a file and a folder"};
        }
        children_[folder].push_back({.name = std::string{name}, .is_directory = false});
        break;
      }
      if (find(path) != nullptr) {
        throw PakError{"pak: " + path + " is a file and a folder"};
      }
      if (children_.try_emplace(path).second) {
        children_[folder].push_back({.name = std::string{name}, .is_directory = true});
      }
      folder = path;
      rest = rest.substr(slash + 1);
    }
  }
}

const PakMount::Entry* PakMount::find(std::string_view path) const {
  const auto it = std::ranges::lower_bound(entries_, path, {}, &Entry::path);
  return it != entries_.end() && it->path == path ? &*it : nullptr;
}

bool PakMount::is_file(std::string_view path) const { return find(path) != nullptr; }

std::vector<std::byte> PakMount::read_file(std::string_view path) const {
  const Entry* entry = find(path);
  if (entry == nullptr) {
    throw PakError{"pak: no file " + std::string{path}};
  }
  const std::string what = "pak: " + entry->path + ": ";
  const std::vector<std::byte> frame = read_range(entry->offset, entry->size);
  // The frame says its own size: compare it before allocating, so a lying index costs nothing.
  if (ZSTD_getFrameContentSize(frame.data(), frame.size()) != entry->raw_size) {
    throw PakError{what + "the data does not match the index"};
  }
  std::vector<std::byte> data(static_cast<std::size_t>(entry->raw_size));
  const std::size_t done = ZSTD_decompress(data.data(), data.size(), frame.data(), frame.size());
  if (ZSTD_isError(done) != 0 || done != data.size()) {
    throw PakError{what + "the data is damaged"};
  }
  if (hash_bytes(data) != entry->hash) {
    throw PakError{what + "the hash does not match"};
  }
  return data;
}

std::optional<std::vector<std::byte>> PakMount::read(std::string_view path) const {
  try {
    return read_file(path);
  } catch (const PakError&) {
    return std::nullopt;
  } catch (const std::bad_alloc&) {
    return std::nullopt;
  }
}

std::optional<std::vector<VfsEntry>> PakMount::list(std::string_view directory) const {
  const auto it = children_.find(directory);
  if (it == children_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<std::string> PakMount::paths() const {
  std::vector<std::string> paths;
  paths.reserve(entries_.size());
  for (const Entry& entry : entries_) {
    paths.push_back(entry.path);
  }
  return paths;
}

bool mount_game_packs(Vfs& vfs, const fs::path& directory) {
  const fs::path game = directory / std::string{kGamePakName};
  std::error_code error;
  if (!fs::is_regular_file(game, error)) {
    return false;
  }
  std::vector<fs::path> patches;
  for (fs::directory_iterator it{directory, error}; !error && it != fs::directory_iterator{};
       it.increment(error)) {
    const std::string name = to_utf8(it->path().filename());
    if (name.starts_with(kPatchPakPrefix) && name.ends_with(kPakExtension) &&
        it->is_regular_file(error)) {
      patches.push_back(it->path());
    }
  }
  std::ranges::sort(patches, {}, [](const fs::path& path) { return to_utf8(path.filename()); });
  vfs.mount(PakMount::open(game));
  for (const fs::path& patch : patches) {
    vfs.mount(PakMount::open(patch));
  }
  return true;
}

}  // namespace vnmaki
