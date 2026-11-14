#include "engine/vfs/pak_writer.hpp"

#include <algorithm>

#include <zstd.h>

#include "engine/assets/import_cache.hpp"
#include "engine/core/byte_stream.hpp"
#include "engine/vfs/pak_mount.hpp"
#include "engine/vfs/path.hpp"

namespace vnmaki {

namespace {

constexpr int kCompressionLevel = 3;

void append_text(std::vector<std::byte>& bytes, std::string_view text) {
  for (const char c : text) {
    bytes.push_back(static_cast<std::byte>(c));
  }
}

}  // namespace

PakWriter::PakWriter(std::ostream& out) : out_(&out) {
  std::vector<std::byte> bytes;
  append_text(bytes, kPakMagic);
  ByteWriter version;
  version.u32(kPakVersion);
  const std::vector<std::byte> version_bytes = version.take();
  bytes.insert(bytes.end(), version_bytes.begin(), version_bytes.end());
  write(bytes);
}

void PakWriter::write(std::span<const std::byte> bytes) {
  // std::byte has no stream support; char* may alias any object.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  out_->write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  if (!*out_) {
    throw PakError{"pak: cannot write"};
  }
  position_ += bytes.size();
}

void PakWriter::add(std::string_view path, std::span<const std::byte> data) {
  if (finished_) {
    throw PakError{"pak: add after finish"};
  }
  if (path.empty() || normalize_virtual_path(path) != path) {
    throw PakError{"pak: '" + std::string{path} + "' is not a canonical virtual path"};
  }
  if (data.size() > kPakMaxFileSize) {
    throw PakError{"pak: " + std::string{path} + " is too large"};
  }
  // A file and a folder cannot have one name.
  const std::string folder = std::string{path} + "/";
  const auto inside = paths_.lower_bound(folder);
  bool clash =
      paths_.contains(std::string{path}) || (inside != paths_.end() && inside->starts_with(folder));
  for (std::size_t slash = path.find('/'); slash != std::string_view::npos && !clash;
       slash = path.find('/', slash + 1)) {
    clash = paths_.contains(std::string{path.substr(0, slash)});
  }
  if (clash) {
    throw PakError{"pak: " + std::string{path} + " clashes with another path"};
  }

  std::vector<std::byte> frame(ZSTD_compressBound(data.size()));
  const std::size_t size =
      ZSTD_compress(frame.data(), frame.size(), data.data(), data.size(), kCompressionLevel);
  if (ZSTD_isError(size) != 0) {
    throw PakError{"pak: compression of " + std::string{path} +
                   " failed: " + ZSTD_getErrorName(size)};
  }
  frame.resize(size);
  entries_.push_back({.path = std::string{path},
                      .offset = position_,
                      .size = size,
                      .raw_size = data.size(),
                      .hash = hash_bytes(data)});
  paths_.insert(std::string{path});
  write(frame);
}

void PakWriter::finish() {
  finished_ = true;
  std::ranges::sort(entries_, {}, &Entry::path);
  const std::uint64_t index_offset = position_;
  ByteWriter index;
  index.u32(static_cast<std::uint32_t>(entries_.size()));
  for (const Entry& entry : entries_) {
    index.string(entry.path);
    index.u64(entry.offset);
    index.u64(entry.size);
    index.u64(entry.raw_size);
    index.u64(entry.hash);
  }
  index.u64(index_offset);
  std::vector<std::byte> bytes = index.take();
  append_text(bytes, kPakFooterMagic);
  write(bytes);
  out_->flush();
  if (!*out_) {
    throw PakError{"pak: cannot write"};
  }
}

}  // namespace vnmaki
