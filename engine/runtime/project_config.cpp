#include "engine/runtime/project_config.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <toml++/toml.hpp>

#include "engine/vfs/vfs.hpp"

namespace vnmaki {

namespace {

constexpr std::int64_t kMaxResolution = 16384;
constexpr Resolution kDefaultResolution{.width = 1920, .height = 1080};

std::string at(const toml::source_position& position) {
  return std::string{kProjectConfigPath} + ":" + std::to_string(position.line) + ":" +
         std::to_string(position.column);
}

[[noreturn]] void fail_at(const toml::node& node, std::string_view reason) {
  throw ProjectConfigError(at(node.source().begin) + ": " + std::string{reason});
}

std::string read_string(const toml::node& node, std::string_view key) {
  const std::optional<std::string> text = node.value<std::string>();
  if (!node.is_string() || !text) {
    fail_at(node, "'" + std::string{key} + "' must be a string");
  }
  return *text;
}

Resolution read_resolution(const toml::node& node) {
  constexpr std::string_view kReason =
      "'resolution' must be [width, height] with whole numbers from 1 to 16384";
  const toml::array* array = node.as_array();
  if (array == nullptr || array->size() != 2) {
    fail_at(node, kReason);
  }
  std::vector<int> sides;
  for (const toml::node& element : *array) {
    const std::optional<std::int64_t> side = element.value_exact<std::int64_t>();
    if (!side || *side < 1 || *side > kMaxResolution) {
      fail_at(node, kReason);
    }
    sides.push_back(static_cast<int>(*side));
  }
  return {.width = sides[0], .height = sides[1]};
}

std::vector<std::string> read_languages(const toml::node& node) {
  constexpr std::string_view kReason = "'languages' must be a non-empty list of language codes";
  const toml::array* array = node.as_array();
  if (array == nullptr || array->empty()) {
    fail_at(node, kReason);
  }
  std::vector<std::string> languages;
  for (const toml::node& element : *array) {
    const std::optional<std::string> code = element.value_exact<std::string>();
    if (!code || code->empty()) {
      fail_at(node, kReason);
    }
    if (std::find(languages.begin(), languages.end(), *code) != languages.end()) {
      fail_at(node, "'languages' lists '" + *code + "' twice");
    }
    languages.push_back(*code);
  }
  return languages;
}

using FontFallbacks = std::map<std::string, std::vector<std::string>>;

/// Reads the `fonts` table. Every language of `fonts.fallback` must be in `languages`.
FontFallbacks read_font_fallbacks(const toml::node& fonts,
                                  const std::vector<std::string>& languages) {
  const toml::table* fonts_table = fonts.as_table();
  if (fonts_table == nullptr) {
    fail_at(fonts, "'fonts' must be a table");
  }
  FontFallbacks chains;
  const toml::node* fallback = fonts_table->get("fallback");
  if (fallback == nullptr) {
    return chains;
  }
  const toml::table* by_language = fallback->as_table();
  if (by_language == nullptr) {
    fail_at(*fallback,
            "'fonts.fallback' must be a table of font path lists, one for each language");
  }
  for (auto&& [key, node] : *by_language) {
    const std::string language{key.str()};
    if (std::find(languages.begin(), languages.end(), language) == languages.end()) {
      fail_at(node,
              "'fonts.fallback' has language '" + language + "', which 'languages' does not list");
    }
    const std::string reason = "'fonts.fallback." + language + "' must be a list of font paths";
    const toml::array* paths = node.as_array();
    if (paths == nullptr) {
      fail_at(node, reason);
    }
    std::vector<std::string>& chain = chains[language];
    chain.reserve(paths->size());
    for (const toml::node& element : *paths) {
      const std::optional<std::string> path = element.value_exact<std::string>();
      if (!path || path->empty()) {
        fail_at(node, reason);
      }
      chain.push_back(*path);
    }
  }
  return chains;
}

}  // namespace

ProjectConfig parse_project_config(std::string_view toml_text) {
  toml::table table;
  try {
    table = toml::parse(toml_text, kProjectConfigPath);
  } catch (const toml::parse_error& error) {
    throw ProjectConfigError(at(error.source().begin) + ": " + std::string{error.description()});
  }

  ProjectConfig config{.title = {},
                       .version = "0.0.0",
                       .resolution = kDefaultResolution,
                       .languages = {"en"},
                       .font_fallbacks = {}};

  const toml::node* title = table.get("title");
  if (title == nullptr) {
    throw ProjectConfigError(std::string{kProjectConfigPath} + ": missing required key 'title'");
  }
  config.title = read_string(*title, "title");
  if (config.title.empty()) {
    fail_at(*title, "'title' must not be empty");
  }
  if (const toml::node* version = table.get("version")) {
    config.version = read_string(*version, "version");
  }
  if (const toml::node* resolution = table.get("resolution")) {
    config.resolution = read_resolution(*resolution);
  }
  if (const toml::node* languages = table.get("languages")) {
    config.languages = read_languages(*languages);
  }
  if (const toml::node* fonts = table.get("fonts")) {
    config.font_fallbacks = read_font_fallbacks(*fonts, config.languages);
  }
  return config;
}

ProjectConfig load_project_config(const Vfs& vfs) {
  const std::optional<std::vector<std::byte>> bytes = vfs.read(kProjectConfigPath);
  if (!bytes) {
    throw ProjectConfigError(std::string{kProjectConfigPath} + ": not found");
  }
  std::string text;
  text.reserve(bytes->size());
  for (const std::byte b : *bytes) {
    text.push_back(static_cast<char>(b));
  }
  return parse_project_config(text);
}

}  // namespace vnmaki
