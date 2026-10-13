#pragma once

// A Vfs over files held in memory, so a test spells out its scripts inline.

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/vfs/vfs.hpp"

namespace vnmaki::test {

class MemoryMount final : public VfsMount {
 public:
  explicit MemoryMount(std::map<std::string, std::string, std::less<>> files)
      : files_(std::move(files)) {}

  [[nodiscard]] bool is_file(std::string_view path) const override {
    return files_.contains(path);
  }

  [[nodiscard]] std::optional<std::vector<std::byte>> read(std::string_view path) const override {
    const auto found = files_.find(path);
    if (found == files_.end()) {
      return std::nullopt;
    }
    std::vector<std::byte> bytes;
    bytes.reserve(found->second.size());
    for (const char c : found->second) {
      bytes.push_back(static_cast<std::byte>(c));
    }
    return bytes;
  }

  [[nodiscard]] std::optional<std::vector<VfsEntry>> list(
      std::string_view directory) const override {
    const std::string prefix = directory.empty() ? "" : std::string{directory} + "/";
    std::vector<VfsEntry> entries;
    bool found = directory.empty();
    for (const auto& [path, contents] : files_) {
      if (!path.starts_with(prefix)) {
        continue;
      }
      found = true;
      const std::string rest = path.substr(prefix.size());
      const std::size_t slash = rest.find('/');
      VfsEntry entry{.name = rest.substr(0, slash), .is_directory = slash != std::string::npos};
      if (std::find(entries.begin(), entries.end(), entry) == entries.end()) {
        entries.push_back(std::move(entry));
      }
    }
    if (!found) {
      return std::nullopt;
    }
    return entries;
  }

 private:
  std::map<std::string, std::string, std::less<>> files_;
};

/// A Vfs with one in-memory mount holding `files` (virtual path -> contents).
inline Vfs memory_vfs(std::map<std::string, std::string, std::less<>> files) {
  Vfs vfs;
  vfs.mount(std::make_unique<MemoryMount>(std::move(files)));
  return vfs;
}

}  // namespace vnmaki::test
