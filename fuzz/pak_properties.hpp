#pragma once

// What must hold when PakMount (engine/vfs/pak_mount.hpp) meets any byte string, and when PakWriter
// and PakMount meet any file contents. The fuzz target fuzz/pak_fuzzer.cpp and a unit test
// (tests/unit/pak_test.cpp) both run it, so the check itself is tested on compilers that build no
// fuzzers.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "engine/vfs/pak_mount.hpp"
#include "engine/vfs/pak_writer.hpp"

namespace vnmaki::fuzz {

inline void require_pak(bool condition) {
  if (!condition) {
    std::abort();
  }
}

/// Walks the folders from the root: every path is reached once, as a file, and read_file gives
/// bytes or throws PakError, never anything else.
inline void check_pak_tree(const PakMount& pak, const std::string& directory,
                           std::size_t& files_seen, std::size_t depth = 0) {
  require_pak(depth < 64);
  const auto entries = pak.list(directory);
  require_pak(entries.has_value());
  for (const VfsEntry& entry : *entries) {
    const std::string path = directory.empty() ? entry.name : directory + "/" + entry.name;
    if (entry.is_directory) {
      check_pak_tree(pak, path, files_seen, depth + 1);
    } else {
      require_pak(pak.is_file(path));
      ++files_seen;
    }
  }
}

/// Opening any bytes either throws PakError or gives a pak whose paths are the files of its
/// folders, and whose files read to bytes or throw PakError.
inline void check_pak_read_properties(std::span<const std::byte> bytes) {
  try {
    const std::unique_ptr<PakMount> pak =
        PakMount::from_bytes(std::vector<std::byte>(bytes.begin(), bytes.end()));
    std::size_t files_seen = 0;
    check_pak_tree(*pak, "", files_seen);
    require_pak(files_seen == pak->paths().size());
    for (const std::string& path : pak->paths()) {
      try {
        static_cast<void>(pak->read_file(path));
      } catch (const PakError&) {
        require_pak(!pak->read(path).has_value());
      }
    }
  } catch (const PakError&) {
    return;
  }
}

/// The bytes as the contents of up to four files (the first byte says how many, each file takes
/// an equal part of the rest): the pak reads back to the same bytes. A pak that is cut short, or
/// has one byte changed (the second and third bytes say where and to what), is read without a
/// crash.
inline void check_pak_round_trip_properties(std::span<const std::byte> bytes) {
  if (bytes.empty()) {
    return;
  }
  constexpr std::size_t kMaxFiles = 4;
  const std::size_t count = std::to_integer<std::size_t>(bytes[0]) % kMaxFiles + 1;
  const std::span<const std::byte> rest = bytes.subspan(1);
  const std::size_t part = rest.size() / count;

  std::ostringstream stream;
  PakWriter writer{stream};
  std::vector<std::string> paths;
  for (std::size_t i = 0; i < count; ++i) {
    paths.push_back("dir/file" + std::to_string(i) + ".bin");
    writer.add(paths.back(), rest.subspan(i * part, part));
  }
  writer.finish();
  const std::string text = stream.str();
  std::vector<std::byte> pak_bytes;
  pak_bytes.reserve(text.size());
  for (const char c : text) {
    pak_bytes.push_back(static_cast<std::byte>(c));
  }

  const std::unique_ptr<PakMount> pak = PakMount::from_bytes(pak_bytes);
  require_pak(pak->paths() == paths);
  for (std::size_t i = 0; i < count; ++i) {
    const std::span<const std::byte> expected = rest.subspan(i * part, part);
    const std::vector<std::byte> actual = pak->read_file(paths[i]);
    require_pak(std::ranges::equal(actual, expected));
  }

  const std::size_t second = bytes.size() > 1 ? std::to_integer<std::size_t>(bytes[1]) : 0;
  const std::size_t third = bytes.size() > 2 ? std::to_integer<std::size_t>(bytes[2]) : 0;
  const std::size_t cut_size = (second * pak_bytes.size()) / 256;
  check_pak_read_properties(std::span{pak_bytes}.first(cut_size));
  pak_bytes[second % pak_bytes.size()] ^= static_cast<std::byte>(third | 1);
  check_pak_read_properties(pak_bytes);
}

}  // namespace vnmaki::fuzz
