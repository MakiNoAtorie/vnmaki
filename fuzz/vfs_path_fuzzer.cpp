// Fuzzes VFS path normalization (engine/vfs/path.hpp). Virtual paths come
// from game scripts and data files, so they are untrusted input.
//
// Besides "does not crash": an input whose ".." climbs above the root is never
// accepted, and every accepted path is already in canonical form (normalizing
// it again changes nothing, and it has no root, no empty, "." or ".." segment,
// no backslash and no forbidden character).

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "engine/vfs/path.hpp"

namespace {

void require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

void check_canonical(std::string_view path) {
  require(path.find('\\') == std::string_view::npos);
  require(path.find(':') == std::string_view::npos);
  require(path.find('\0') == std::string_view::npos);
  if (path.empty()) {
    return;
  }
  require(!path.starts_with('/'));
  require(!path.ends_with('/'));
  std::size_t begin = 0;
  while (begin <= path.size()) {
    std::size_t end = path.find('/', begin);
    if (end == std::string_view::npos) {
      end = path.size();
    }
    const std::string_view segment = path.substr(begin, end - begin);
    require(!segment.empty());
    require(segment != "." && segment != "..");
    begin = end + 1;
  }
}

// An independent model of "does this path climb above the root?": walk the
// segments, counting depth, and report whether ".." ever goes below zero.
bool climbs_above_root(std::string_view path) {
  long depth = 0;
  std::size_t begin = 0;
  while (begin <= path.size()) {
    std::size_t end = path.find_first_of("/\\", begin);
    if (end == std::string_view::npos) {
      end = path.size();
    }
    const std::string_view segment = path.substr(begin, end - begin);
    if (segment == "..") {
      --depth;
    } else if (!segment.empty() && segment != ".") {
      ++depth;
    }
    if (depth < 0) {
      return true;
    }
    begin = end + 1;
  }
  return false;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::uint8_t> bytes{data, size};
  std::string input;
  input.reserve(size);
  for (const std::uint8_t byte : bytes) {
    input.push_back(static_cast<char>(byte));
  }

  const std::optional<std::string> normalized = vnmaki::normalize_virtual_path(input);
  if (!normalized) {
    return 0;
  }
  require(!climbs_above_root(input));
  check_canonical(*normalized);
  require(vnmaki::normalize_virtual_path(*normalized) == normalized);
  return 0;
}
