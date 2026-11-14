#include "engine/runtime/new_project.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "engine/core/utf8_path.hpp"
#include "engine/runtime/project_config.hpp"

namespace vnmaki {

namespace {

namespace fs = std::filesystem;

constexpr unsigned char kFirstPrintable = 0x20;
constexpr unsigned char kDelete = 0x7F;

bool has_control_character(std::string_view text) {
  return std::ranges::any_of(text, [](char c) {
    const auto byte = static_cast<unsigned char>(c);
    return byte < kFirstPrintable || byte == kDelete;
  });
}

std::string escaped(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const char c : text) {
    if (c == '\\' || c == '"') {
      result += '\\';
    }
    result += c;
  }
  return result;
}

std::string read_file(const fs::path& path) {
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    throw NewProjectError{"cannot read " + to_utf8(path)};
  }
  return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void write_file(const fs::path& path, std::string_view content) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  stream.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!stream) {
    throw NewProjectError{"cannot write " + to_utf8(path)};
  }
}

bool is_text_template(const fs::path& path) {
  const fs::path extension = path.extension();
  return extension == ".toml" || extension == ".lua";
}

std::string with_title(std::string text, std::string_view title) {
  const std::string value = escaped(title);
  for (std::size_t at = text.find(kTitlePlaceholder); at != std::string::npos;
       at = text.find(kTitlePlaceholder, at + value.size())) {
    text.replace(at, kTitlePlaceholder.size(), value);
  }
  return text;
}

void check_target(const fs::path& target_dir) {
  std::error_code error;
  if (!fs::exists(target_dir, error)) {
    return;
  }
  if (!fs::is_directory(target_dir, error) || !fs::is_empty(target_dir, error)) {
    throw NewProjectError{to_utf8(target_dir) + " is not an empty folder"};
  }
}

constexpr std::string_view kAddonManifest = "addon.toml";

std::string trimmed(std::string_view text) {
  const std::size_t first = text.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) {
    return {};
  }
  return std::string{text.substr(first, text.find_last_not_of(" \t\r") + 1 - first)};
}

std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    lines.push_back(text.substr(start, end == std::string::npos ? end : end - start));
    start = end == std::string::npos ? text.size() : end + 1;
  }
  return lines;
}

// The `language` and the tables of addon.toml: the key lines of each table, in order.
struct Manifest {
  std::string language;
  std::vector<std::pair<std::string, std::vector<std::string>>> tables;
};

Manifest read_manifest(const fs::path& file) {
  Manifest manifest;
  for (const std::string& raw : lines_of(read_file(file))) {
    const std::string line = trimmed(raw);
    if (line.empty() || line.starts_with("#")) {
      continue;
    }
    if (line.starts_with("[")) {
      manifest.tables.emplace_back(line, std::vector<std::string>{});
    } else if (!manifest.tables.empty()) {
      manifest.tables.back().second.push_back(line);
    } else if (line.starts_with("language")) {
      const std::size_t open = line.find('"');
      const std::size_t close = line.rfind('"');
      if (open != std::string::npos && close > open) {
        manifest.language = line.substr(open + 1, close - open - 1);
      }
    }
  }
  if (manifest.language.empty()) {
    throw NewProjectError{to_utf8(file) + " has no line: language = \"<code>\""};
  }
  return manifest;
}

// The language goes before the `]` that ends the line `languages = [...]`.
bool add_language(std::vector<std::string>& lines, const std::string& language) {
  for (std::string& line : lines) {
    if (!trimmed(line).starts_with("languages")) {
      continue;
    }
    const std::string quoted = "\"" + language + "\"";
    const std::size_t close = line.rfind(']');
    if (close == std::string::npos || line.find(quoted) != std::string::npos) {
      return false;
    }
    line.insert(close, ", " + quoted);
    return true;
  }
  return false;
}

void add_keys(std::vector<std::string>& lines, const std::string& header,
              const std::vector<std::string>& keys) {
  auto at =
      std::ranges::find_if(lines, [&](const std::string& line) { return trimmed(line) == header; });
  if (at == lines.end()) {
    lines.emplace_back();
    lines.push_back(header);
    at = lines.end() - 1;
  }
  lines.insert(at + 1, keys.begin(), keys.end());
}

}  // namespace

std::string project_title(const fs::path& name) {
  fs::path folder = fs::absolute(name).lexically_normal();
  if (!folder.has_filename()) {
    folder = folder.parent_path();
  }
  const std::string title = to_utf8(folder.filename());
  if (title.empty()) {
    throw NewProjectError{"'" + to_utf8(name) + "' has no folder name to use as the title"};
  }
  if (has_control_character(title)) {
    throw NewProjectError{"the name has a control character"};
  }
  return title;
}

void create_project(const fs::path& template_dir, const fs::path& target_dir,
                    std::string_view title) {
  if (has_control_character(title)) {
    throw NewProjectError{"the title has a control character"};
  }
  check_target(target_dir);
  try {
    fs::create_directories(target_dir);
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(template_dir)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const fs::path file = fs::relative(entry.path(), template_dir);
      fs::create_directories((target_dir / file).parent_path());
      if (is_text_template(file)) {
        write_file(target_dir / file, with_title(read_file(template_dir / file), title));
      } else {
        fs::copy_file(template_dir / file, target_dir / file);
      }
    }
  } catch (const fs::filesystem_error& error) {
    throw NewProjectError{error.what()};
  }
}

std::optional<fs::path> find_template(std::string_view name, const fs::path& executable_dir,
                                      const fs::path& source_templates_dir) {
  for (const fs::path& root : {executable_dir / "templates", source_templates_dir}) {
    std::error_code error;
    if (fs::is_regular_file(root / name / kProjectConfigPath, error)) {
      return root / name;
    }
  }
  return std::nullopt;
}

std::optional<fs::path> find_addon(std::string_view name, const fs::path& executable_dir,
                                   const fs::path& source_templates_dir) {
  if (name.empty() || name.find_first_of("/\\.:") != std::string_view::npos) {
    return std::nullopt;
  }
  for (const fs::path& root : {executable_dir / "templates", source_templates_dir}) {
    std::error_code error;
    if (fs::is_regular_file(root / "addons" / name / kAddonManifest, error)) {
      return root / "addons" / name;
    }
  }
  return std::nullopt;
}

std::vector<std::string> list_addons(const fs::path& executable_dir,
                                     const fs::path& source_templates_dir) {
  std::set<std::string> names;
  for (const fs::path& root : {executable_dir / "templates", source_templates_dir}) {
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator(root / "addons", error)) {
      const std::string name = to_utf8(entry.path().filename());
      if (find_addon(name, executable_dir, source_templates_dir)) {
        names.insert(name);
      }
    }
  }
  return {names.begin(), names.end()};
}

void apply_addon(const fs::path& addon_dir, const fs::path& project_dir) {
  const Manifest manifest = read_manifest(addon_dir / kAddonManifest);
  std::vector<std::string> lines = lines_of(read_file(project_dir / kProjectConfigPath));
  if (!add_language(lines, manifest.language)) {
    throw NewProjectError{"project.toml has the language '" + manifest.language +
                          "' already, or has no line: languages = [...]"};
  }
  for (const auto& [header, keys] : manifest.tables) {
    add_keys(lines, header, keys);
  }
  try {
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(addon_dir)) {
      const fs::path file = fs::relative(entry.path(), addon_dir);
      if (!entry.is_regular_file() || file == kAddonManifest) {
        continue;
      }
      if (fs::exists(project_dir / file)) {
        throw NewProjectError{to_utf8(file) + " exists in the game already"};
      }
      files.push_back(file);
    }
    for (const fs::path& file : files) {
      fs::create_directories((project_dir / file).parent_path());
      fs::copy_file(addon_dir / file, project_dir / file);
    }
  } catch (const fs::filesystem_error& error) {
    throw NewProjectError{error.what()};
  }
  std::string text;
  for (const std::string& line : lines) {
    text += line + '\n';
  }
  write_file(project_dir / kProjectConfigPath, text);
}

}  // namespace vnmaki
