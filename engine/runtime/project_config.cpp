#include "engine/runtime/project_config.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

#include "engine/vfs/vfs.hpp"

namespace vnmaki {

namespace {

constexpr std::int64_t kMaxResolution = 16384;
// Each rollback step keeps a snapshot, so the depth has a limit.
constexpr std::int64_t kMaxRollbackDepth = 10000;
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

// Reads `[text_direction]` or `[styles]`: a table with one string for each language. `parse` turns
// the string into the value, or returns false.
template <typename T, typename Parse>
std::map<std::string, T> read_language_table(const toml::node& node, std::string_view name,
                                             const std::vector<std::string>& languages,
                                             std::string_view expected, Parse parse) {
  const toml::table* table = node.as_table();
  if (table == nullptr) {
    fail_at(node, "'" + std::string{name} + "' must be a table with a value for each language");
  }
  std::map<std::string, T> result;
  for (auto&& [key, value] : *table) {
    const std::string language{key.str()};
    if (std::ranges::find(languages, language) == languages.end()) {
      fail_at(value, "'" + std::string{name} + "' has language '" + language +
                         "', which 'languages' does not list");
    }
    const std::optional<std::string> text = value.value_exact<std::string>();
    T parsed{};
    if (!text || !parse(*text, parsed)) {
      fail_at(value,
              "'" + std::string{name} + "." + language + "' must be " + std::string{expected});
    }
    result[language] = std::move(parsed);
  }
  return result;
}

std::vector<AutoSaveEvent> read_auto_save(const toml::node& node) {
  constexpr std::string_view kReason = R"('auto_save' must be a list of "label" and "menu")";
  const toml::array* array = node.as_array();
  if (array == nullptr) {
    fail_at(node, kReason);
  }
  std::vector<AutoSaveEvent> events;
  events.reserve(array->size());
  for (const toml::node& element : *array) {
    const std::optional<std::string> name = element.value_exact<std::string>();
    AutoSaveEvent event{};
    if (name == "label") {
      event = AutoSaveEvent::kLabel;
    } else if (name == "menu") {
      event = AutoSaveEvent::kMenu;
    } else {
      fail_at(node, kReason);
    }
    if (std::ranges::find(events, event) != events.end()) {
      fail_at(node, "'auto_save' lists '" + name.value_or("") + "' twice");
    }
    events.push_back(event);
  }
  return events;
}

std::vector<std::string> read_gallery(const toml::node& node) {
  constexpr std::string_view kReason = "'gallery' must be a list of image names";
  const toml::array* array = node.as_array();
  if (array == nullptr) {
    fail_at(node, kReason);
  }
  std::vector<std::string> images;
  images.reserve(array->size());
  for (const toml::node& element : *array) {
    const std::optional<std::string> name = element.value_exact<std::string>();
    if (!name || name->empty()) {
      fail_at(node, kReason);
    }
    if (std::ranges::find(images, *name) != images.end()) {
      fail_at(node, "'gallery' lists '" + *name + "' twice");
    }
    images.push_back(*name);
  }
  return images;
}

std::size_t read_count(const toml::node& node, std::string_view key) {
  const std::optional<std::int64_t> depth = node.value_exact<std::int64_t>();
  if (!depth || *depth < 0 || *depth > kMaxRollbackDepth) {
    fail_at(node, "'" + std::string{key} + "' must be a whole number from 0 to " +
                      std::to_string(kMaxRollbackDepth));
  }
  return static_cast<std::size_t>(*depth);
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
  if (const toml::node* directions = table.get("text_direction")) {
    config.text_directions = read_language_table<TextDirection>(
        *directions, "text_direction", config.languages, R"("ltr" or "rtl")",
        [](const std::string& text, TextDirection& direction) {
          direction = text == "rtl" ? TextDirection::kRtl : TextDirection::kLtr;
          return text == "rtl" || text == "ltr";
        });
  }
  if (const toml::node* styles = table.get("styles")) {
    config.language_styles =
        read_language_table<std::string>(*styles, "styles", config.languages, "a file path",
                                         [](const std::string& text, std::string& path) {
                                           path = text;
                                           return !text.empty();
                                         });
  }
  if (const toml::node* auto_save = table.get("auto_save")) {
    config.auto_save = read_auto_save(*auto_save);
  }
  if (const toml::node* gallery = table.get("gallery")) {
    config.gallery = read_gallery(*gallery);
  }
  if (const toml::node* depth = table.get("rollback_depth")) {
    config.rollback_depth = read_count(*depth, "rollback_depth");
  }
  if (const toml::node* length = table.get("backlog_length")) {
    config.backlog_length = read_count(*length, "backlog_length");
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
