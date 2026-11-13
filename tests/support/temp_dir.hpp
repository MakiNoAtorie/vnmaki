#pragma once

// A temp path that no other test process uses. Several worktrees and ctest runs can run the same
// test at once, so a fixed name under the system temp directory is a race.

#include <filesystem>
#include <random>
#include <string>
#include <string_view>

namespace vnmaki::test {

/// `<temp>/<prefix><name>_<random>`. The random part is one number for the whole process, so the
/// same name gives the same path inside a run.
inline std::filesystem::path unique_temp_path(std::string_view prefix, std::string_view name) {
  static const std::string run = std::to_string(std::random_device{}());
  return std::filesystem::temp_directory_path() /
         (std::string{prefix} + std::string{name} + "_" + run);
}

}  // namespace vnmaki::test
