#pragma once

// A game made like `vnmaki new` does it: the template `default` and, when asked, language add-ons
// of templates/addons/ (`apply_addon`). The template has English only, so a test that needs
// Japanese asks for the add-on "ja".

#include <filesystem>
#include <initializer_list>
#include <string_view>
#include <system_error>

#include "engine/runtime/new_project.hpp"
#include "tests/support/temp_dir.hpp"

namespace vnmaki::test {

inline std::filesystem::path template_dir() {
  return std::filesystem::path{VNMAKI_TEMPLATES_DIR} / "default";
}

inline std::filesystem::path addon_dir(std::string_view name) {
  return std::filesystem::path{VNMAKI_TEMPLATES_DIR} / "addons" / name;
}

inline void make_template_project(const std::filesystem::path& target, std::string_view title,
                                  std::initializer_list<std::string_view> addons = {}) {
  create_project(template_dir(), target, title);
  for (const std::string_view addon : addons) {
    apply_addon(addon_dir(addon), target);
  }
}

/// A game of `make_template_project` in the temp directory, removed when the object dies.
class TemplateProject {
 public:
  explicit TemplateProject(std::string_view name,
                           std::initializer_list<std::string_view> addons = {})
      : path_(unique_temp_path("vnmaki_template_", name)) {
    std::filesystem::remove_all(path_);
    make_template_project(path_, name, addons);
  }
  ~TemplateProject() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  TemplateProject(const TemplateProject&) = delete;
  TemplateProject& operator=(const TemplateProject&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace vnmaki::test
