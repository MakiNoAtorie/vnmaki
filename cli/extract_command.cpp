#include "cli/extract_command.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "engine/core/utf8_path.hpp"
#include "engine/l10n/extract.hpp"
#include "engine/l10n/po.hpp"
#include "engine/runtime/project_config.hpp"
#include "engine/tools/story_graph.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki::cli {

namespace {

constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

namespace fs = std::filesystem;

struct Output {
  std::string path{};  ///< The virtual path, e.g. "translations/ja.po".
  PoFile file{};
};

// The language is a file name: letters, digits, '-' and '_' only.
bool is_language_code(std::string_view language) {
  return !language.empty() && std::ranges::all_of(language, [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_';
         });
}

std::optional<std::string> read_text(const fs::path& path) {
  std::ifstream in{path, std::ios::binary};
  if (!in) {
    return std::nullopt;
  }
  return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// Writes next to the file and renames, so that a failed write keeps the old translations.
bool write_text(const fs::path& path, const std::string& text) {
  std::error_code ignored;
  fs::create_directories(path.parent_path(), ignored);
  fs::path temporary = path;
  temporary += ".tmp";
  {
    std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
    out << text;
    if (!out.flush()) {
      return false;
    }
  }
  std::error_code error;
  fs::rename(temporary, path, error);
  return !error;
}

std::string summary(const Output& output) {
  std::size_t strings = 0;
  std::size_t fuzzy = 0;
  std::size_t obsolete = 0;
  for (const PoEntry& entry : output.file.entries) {
    if (entry.context.empty() && entry.id.empty()) {
      continue;
    }
    strings += entry.obsolete ? 0 : 1;
    fuzzy += entry.fuzzy() && !entry.obsolete ? 1 : 0;
    obsolete += entry.obsolete ? 1 : 0;
  }
  std::string line = output.path + ": " + std::to_string(strings) + " strings";
  if (output.path.ends_with(".po")) {
    line += ", " + std::to_string(fuzzy) + " fuzzy, " + std::to_string(obsolete) + " obsolete";
  }
  return line + "\n";
}

}  // namespace

int extract_command(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  if (args.size() != 1 || args.front().starts_with("-")) {
    err << "usage:" << kExtractUsage.substr(std::string_view{"      "}.size());
    return kExitUsage;
  }
  const fs::path directory = utf8_path(args.front());
  Vfs vfs;
  if (!vfs.mount(make_directory_mount(directory))) {
    err << "vnmaki: " << to_utf8(directory) << " is not a directory\n";
    return kExitError;
  }
  ProjectConfig config;
  try {
    config = load_project_config(vfs);
  } catch (const ProjectConfigError& error) {
    err << "vnmaki: " << error.what() << "\n";
    return kExitError;
  }

  // Work out every file first, so that an unreadable .po file leaves all files as they were.
  const PoFile tmpl = extract_template(read_scripts(vfs));
  std::vector<Output> outputs;
  outputs.push_back({.path = std::string{kTemplatePath}, .file = tmpl});
  for (std::size_t i = 1; i < config.languages.size(); ++i) {  // [0] is the source language
    const std::string& language = config.languages[i];
    if (!is_language_code(language)) {
      err << "vnmaki: project.toml: '" << language << "' is not a language code\n";
      return kExitError;
    }
    const std::string path = translation_path(language);
    PoFile old;
    if (const std::optional<std::string> text = read_text(directory / utf8_path(path))) {
      try {
        old = parse_po(*text);
      } catch (const PoError& error) {
        err << "vnmaki: " << path << ":" << error.line() << ": " << error.what() << "\n";
        return kExitError;
      }
    }
    outputs.push_back({.path = path, .file = update_translation(tmpl, old, language)});
  }
  for (const Output& output : outputs) {
    if (!write_text(directory / utf8_path(output.path), write_po(output.file))) {
      err << "vnmaki: cannot write " << to_utf8(directory / utf8_path(output.path)) << "\n";
      return kExitError;
    }
    out << summary(output);
  }
  return 0;
}

}  // namespace vnmaki::cli
