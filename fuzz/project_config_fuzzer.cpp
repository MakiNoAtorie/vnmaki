// Fuzzes project.toml parsing (engine/runtime/project_config.hpp).
//
// Besides "does not crash": parsing either throws ProjectConfigError with a
// message naming project.toml, or returns a config that satisfies the schema
// (non-empty title, resolution in range, a non-empty list of distinct
// non-empty language codes, font fallback chains only for those languages and
// without empty paths).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

#include "engine/runtime/project_config.hpp"

namespace {

constexpr int kMaxResolution = 16384;

void require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

void check(const vnmaki::ProjectConfig& config) {
  require(!config.title.empty());
  require(config.resolution.width >= 1 && config.resolution.width <= kMaxResolution);
  require(config.resolution.height >= 1 && config.resolution.height <= kMaxResolution);
  require(!config.languages.empty());
  for (auto it = config.languages.begin(); it != config.languages.end(); ++it) {
    require(!it->empty());
    require(std::find(std::next(it), config.languages.end(), *it) == config.languages.end());
  }
  for (const auto& [language, paths] : config.font_fallbacks) {
    require(std::find(config.languages.begin(), config.languages.end(), language) !=
            config.languages.end());
    require(std::none_of(paths.begin(), paths.end(),
                         [](const std::string& path) { return path.empty(); }));
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::uint8_t> bytes{data, size};
  std::string text;
  text.reserve(size);
  for (const std::uint8_t byte : bytes) {
    text.push_back(static_cast<char>(byte));
  }
  try {
    check(vnmaki::parse_project_config(text));
  } catch (const vnmaki::ProjectConfigError& error) {
    require(std::string_view{error.what()}.starts_with("project.toml"));
  }
  return 0;
}
