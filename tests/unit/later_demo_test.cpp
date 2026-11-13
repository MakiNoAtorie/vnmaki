// The Phase 7 demo (docs/PLAN.md task 53), part 1: the screen editor on the template. The template
// of `vnmaki new` is made in a temporary folder and its quick menu is shown as the story window
// shows it. The editor changes the `gap` of the row of buttons through its functions, and hot
// reload gives new node rectangles. The engine's own quick_menu is a function, so it is read-only;
// the template has its own table form of it (templates/default/scripts/ui.lua). No window or GPU.
// The launcher part is in launcher_test.cpp, the vertical say box is in
// tests/render/template_vertical_render_test.cpp.

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/runtime/new_project.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/tools/hot_reload.hpp"
#include "engine/tools/log_buffer.hpp"
#include "engine/tools/screen_editor.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/ui/ui_node.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/temp_dir.hpp"

namespace {

namespace fs = std::filesystem;

constexpr vnmaki::Size kCanvas{.width = 1280, .height = 720};  // resolution of the template
constexpr std::string_view kScreen = "quick_menu";
constexpr std::string_view kUi = "scripts/ui.lua";
constexpr std::string_view kOldGap = "gap = 6,";
constexpr std::string_view kNewGap = "gap = 14,";
constexpr float kOldGapValue = 6;
constexpr float kNewGapValue = 14;
constexpr std::size_t kButtons = 6;  // Save, Load, Auto, Skip, Log, Prefs
// Layout snaps a rectangle to whole pixels, so a sum of widths and gaps is off by up to a pixel.
constexpr float kSnap = 1;

std::string read_file(const fs::path& path) {
  std::ifstream stream{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

struct TempProject {
  explicit TempProject(const std::string& name)
      : root(vnmaki::test::unique_temp_path("vnmaki_later_demo_", name)) {
    fs::remove_all(root);
  }
  TempProject(const TempProject&) = delete;
  TempProject& operator=(const TempProject&) = delete;
  ~TempProject() {
    std::error_code ignored;  // a folder left in the temp directory is not worth a failure
    fs::remove_all(root, ignored);
  }

  fs::path root;
};

// The template as `vnmaki run` plays it, with the quick menu on top, hot reload and the editor.
class TemplateGame {
 public:
  explicit TemplateGame(const std::string& name)
      : temp_(name),
        project_(temp_.root / "my_game"),
        vfs_(prepare()),
        scene_(vfs_, kCanvas),
        ui_(kCanvas),
        runtime_(vfs_, {.screens = &ui_,
                        .canvas_width = kCanvas.width,
                        .canvas_height = kCanvas.height}),
        hot_(runtime_, scene_, jobs_, ui_, log_),
        editor_(vfs_, project_, ui_) {
    runtime_.load_scripts();
    // The story window opens the quick menu itself, and again after each reload of the scripts.
    hot_.set_on_scripts_reloaded([this] { open_quick_menu(); });
    open_quick_menu();
  }
  TemplateGame(const TemplateGame&) = delete;
  TemplateGame& operator=(const TemplateGame&) = delete;

  [[nodiscard]] const fs::path& project() const { return project_; }
  [[nodiscard]] vnmaki::UiHost& ui() { return ui_; }
  [[nodiscard]] vnmaki::HotReload& hot() { return hot_; }
  [[nodiscard]] vnmaki::ScreenEditor& editor() { return editor_; }

 private:
  // `vnmaki new`, then mounts the folder as `vnmaki run` does.
  vnmaki::Vfs prepare() {
    vnmaki::create_project(fs::path{VNMAKI_TEMPLATES_DIR} / "default", project_, "my_game");
    vnmaki::Vfs vfs;
    REQUIRE(vfs.mount(vnmaki::make_directory_mount(project_)));
    return vfs;
  }

  void open_quick_menu() {
    vnmaki::ScreenNodeDef tree;
    REQUIRE(runtime_.build_screen(kScreen, {}, tree).empty());
    REQUIRE(ui_.show_screen(kScreen, tree).empty());
  }

  TempProject temp_;
  fs::path project_;
  vnmaki::Vfs vfs_;
  vnmaki::Scene scene_;
  vnmaki::JobSystem jobs_{0};
  vnmaki::LogBuffer log_;
  vnmaki::UiHost ui_;
  vnmaki::ScriptRuntime runtime_;
  vnmaki::HotReload hot_;
  vnmaki::ScreenEditor editor_;
};

// The row of buttons: the root is a vbox with one hbox, which has the buttons.
const vnmaki::UiNode& row_of(vnmaki::UiHost& ui) {
  const vnmaki::UiNode* root = ui.find(kScreen);
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 1);
  const vnmaki::UiNode& row = root->children.front();
  REQUIRE(row.children.size() == kButtons);
  return row;
}

// The space between button i and button i + 1, from the rectangles.
float space_after(const vnmaki::UiNode& row, std::size_t i) {
  const vnmaki::RectF& left = row.children[i].rect;
  return row.children[i + 1].rect.x - (left.x + left.width);
}

bool near(float actual, float expected) { return std::abs(actual - expected) <= kSnap; }

vnmaki::ScreenEditor::Field& gap_field(vnmaki::ScreenEditor& editor) {
  for (vnmaki::ScreenEditor::Field& field : editor.fields()) {
    if (field.key == "gap") {
      return field;
    }
  }
  FAIL("the row has no gap field");
  return editor.fields().front();
}

}  // namespace

TEST_CASE("later demo: the template quick menu gap changes through the editor and shows after reload") {
  TemplateGame game{"quick_menu_gap"};
  const vnmaki::UiNode& before = row_of(game.ui());
  const vnmaki::RectF old_row = before.rect;
  std::vector<vnmaki::RectF> old_buttons;
  for (const vnmaki::UiNode& button : before.children) {
    old_buttons.push_back(button.rect);
  }
  for (std::size_t i = 0; i + 1 < kButtons; ++i) {
    CHECK(near(space_after(before, i), kOldGapValue));
  }

  // The row is the first node under the root of the table-form screen, so the editor can write it.
  vnmaki::ScreenEditor& editor = game.editor();
  REQUIRE(editor.select(std::string{kScreen}, {0}));
  REQUIRE(editor.problem().empty());
  CHECK(editor.source_kind() == "hbox");
  vnmaki::ScreenEditor::Field& gap = gap_field(editor);
  REQUIRE(gap.editable);
  CHECK(gap.original == "6");
  gap.value = "14";

  const std::string source = read_file(game.project() / kUi);
  REQUIRE(editor.save().empty());
  // Only the bytes of the literal changed.
  std::string expected = source;
  const std::size_t at = expected.find(kOldGap);
  REQUIRE(at != std::string::npos);
  expected.replace(at, kOldGap.size(), kNewGap);
  CHECK(read_file(game.project() / kUi) == expected);
  // Not yet on screen: the host has the old tree until hot reload runs.
  CHECK(near(space_after(row_of(game.ui()), 0), kOldGapValue));

  game.hot().apply(std::vector<std::string>{std::string{kUi}});
  CHECK(game.hot().banner().empty());

  // The nodes have new rectangles: the buttons keep their size and are 8 pixels further apart,
  // and the bar aligns to the end, so the row grows to the left.
  const vnmaki::UiNode& row = row_of(game.ui());
  for (std::size_t i = 0; i + 1 < kButtons; ++i) {
    CHECK(near(space_after(row, i), kNewGapValue));
  }
  for (std::size_t i = 0; i < kButtons; ++i) {
    CHECK(near(row.children[i].rect.width, old_buttons[i].width));
    CHECK(near(row.children[i].rect.height, old_buttons[i].height));
  }
  const float grown = (kNewGapValue - kOldGapValue) * static_cast<float>(kButtons - 1);
  CHECK(near(row.rect.width, old_row.width + grown));
  CHECK(near(row.rect.x, old_row.x - grown));
  CHECK(near(row.rect.y, old_row.y));
  CHECK(near(row.children.back().rect.x, old_buttons.back().x));
  CHECK(near(row.children.front().rect.x, old_buttons.front().x - grown));

  // The selection finds its node again in the new tree.
  REQUIRE(editor.select(std::string{kScreen}, {0}));
  CHECK(gap_field(editor).original == "14");
}

TEST_CASE("later demo: the engine's own quick menu is a function, so the editor leaves it alone") {
  TemplateGame game{"quick_menu_default"};
  // A project with no ui.lua has the engine's quick_menu, which is a function.
  REQUIRE(fs::remove(game.project() / kUi));
  game.hot().apply(std::vector<std::string>{std::string{kUi}});
  REQUIRE(game.hot().banner().empty());
  REQUIRE(game.editor().select(std::string{kScreen}, {0}));
  CHECK_FALSE(game.editor().problem().empty());
  CHECK(game.editor().fields().empty());
}
