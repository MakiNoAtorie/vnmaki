#pragma once

// `vnmaki new`: a game folder made from a template (docs/SPEC.md section 19). A template is a
// folder with a project.toml, such as templates/default/.

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vnmaki {

/// The text in a template's `.toml` and `.lua` files that `create_project` replaces with the
/// game's title.
inline constexpr std::string_view kTitlePlaceholder = "{{title}}";

/// The target is not an empty folder, the title cannot be used, or a file cannot be copied.
class NewProjectError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// The title that `vnmaki new <name>` gives the game: the last part of `name`, or of the folder
/// that it names when it ends with a separator or is ".". Throws NewProjectError if there is none
/// or it has a control character.
[[nodiscard]] std::string project_title(const std::filesystem::path& name);

/// Copies the files of `template_dir` into `target_dir`, which must not exist or be an empty
/// folder. In `.toml` and `.lua` files, every kTitlePlaceholder becomes `title`, escaped for a
/// quoted string of TOML or Lua (a backslash or quote gets a backslash). Other files are copied as
/// they are. Throws NewProjectError.
void create_project(const std::filesystem::path& template_dir,
                    const std::filesystem::path& target_dir, std::string_view title);

/// The folder of the template `name`, or nothing. It looks for `<executable_dir>/templates/<name>`
/// (an installed engine) and then `<source_templates_dir>/<name>` (a build from the source tree).
/// A folder counts when it has a project.toml.
[[nodiscard]] std::optional<std::filesystem::path> find_template(
    std::string_view name, const std::filesystem::path& executable_dir,
    const std::filesystem::path& source_templates_dir);

/// The folder of the add-on `name` (a language such as `ja`): `<executable_dir>/templates/addons/
/// <name>` and then `<source_templates_dir>/addons/<name>`. A folder counts when it has an
/// `addon.toml`. The name is a file name, so a name with a separator or a dot finds nothing.
[[nodiscard]] std::optional<std::filesystem::path> find_addon(
    std::string_view name, const std::filesystem::path& executable_dir,
    const std::filesystem::path& source_templates_dir);

/// The names of the add-ons that `find_addon` finds, sorted, without repeats.
[[nodiscard]] std::vector<std::string> list_addons(
    const std::filesystem::path& executable_dir, const std::filesystem::path& source_templates_dir);

/// Adds the add-on in `addon_dir` to the game in `project_dir`. `addon.toml` has `language =
/// "<code>"` and tables (`[vertical]`, `[fonts.fallback]`, ...) with keys. The code goes to the end
/// of `languages` in project.toml, and the keys of each table go to that table of project.toml
/// (the table is added at the end when it is missing). Comments of `addon.toml` are not copied. The
/// other files of the add-on are copied. The other lines of project.toml stay as they are. Throws
/// NewProjectError, before it writes anything, when the game has the language already or has a
/// file of the add-on.
void apply_addon(const std::filesystem::path& addon_dir, const std::filesystem::path& project_dir);

}  // namespace vnmaki
