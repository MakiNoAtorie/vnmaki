#include "engine/runtime/new_project.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

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
    throw NewProjectError{"cannot read " + path.string()};
  }
  return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void write_file(const fs::path& path, std::string_view content) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  stream.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!stream) {
    throw NewProjectError{"cannot write " + path.string()};
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
    throw NewProjectError{target_dir.string() + " is not an empty folder"};
  }
}

}  // namespace

std::string project_title(const fs::path& name) {
  fs::path folder = fs::absolute(name).lexically_normal();
  if (!folder.has_filename()) {
    folder = folder.parent_path();
  }
  const std::string title = folder.filename().string();
  if (title.empty()) {
    throw NewProjectError{"'" + name.string() + "' has no folder name to use as the title"};
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

}  // namespace vnmaki
