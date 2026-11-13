// The screen editor (docs/SPEC.md section 18): the source span of each literal in a screen table,
// the rewrite of exactly those bytes, the map from a node on screen back to its table, and a hot
// reload that shows the saved change. The project is made by tools/generators/
// screen_editor_fixtures.py; each test works on a copy of it in the temp folder. No window or GPU.

#include "engine/tools/screen_editor.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/tools/hot_reload.hpp"
#include "engine/tools/log_buffer.hpp"
#include "engine/tools/script_scan.hpp"
#include "engine/tools/story_graph.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/ui/ui_node.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/temp_dir.hpp"

namespace {

namespace fs = std::filesystem;
using vnmaki::ScanValueKind;
using vnmaki::SourcePath;
using vnmaki::UiNode;

constexpr vnmaki::Size kCanvas{.width = 1920, .height = 1080};
constexpr std::string_view kUi = "scripts/ui.lua";
// What the generator's slots say the engine writes (the same strings as there).
constexpr std::string_view kNewHello =
    "Line \"two\" \xC3\x9C"
    "n\xC3\xAF \xE2\x9C\x93";
constexpr std::string_view kNewSound = "Line1\nLine2";
constexpr float kOne = 1;
constexpr int kByteValues = 256;

const fs::path kFixtures = fs::path{VNMAKI_FIXTURES_DIR} / "screen_editor";

std::string read_bytes(const fs::path& path) {
  std::ifstream file{path, std::ios::binary};
  REQUIRE(file);
  return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void write_bytes(const fs::path& path, const std::string& text) {
  std::ofstream out{path, std::ios::binary | std::ios::trunc};
  out << text;
}

// A copy of the fixture project that the test may change.
class TempProject {
 public:
  explicit TempProject(std::string_view name)
      : root_(vnmaki::test::unique_temp_path("vnmaki_screen_editor_", name)) {
    fs::remove_all(root_);
    fs::copy(kFixtures / "project", root_, fs::copy_options::recursive);
  }
  TempProject(const TempProject&) = delete;
  TempProject& operator=(const TempProject&) = delete;
  ~TempProject() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }
  [[nodiscard]] const fs::path& root() const { return root_; }
  [[nodiscard]] fs::path ui() const { return root_ / kUi; }

 private:
  fs::path root_;
};

// The fixture game at its start: the screens menu, cells, dyn and fn are shown.
struct Game {
  explicit Game(std::string_view name)
      : project(name),
        vfs(make_vfs(project)),
        scene(vfs, kCanvas),
        ui(kCanvas),
        runtime(vfs, {.screens = &ui}),
        hot(runtime, scene, jobs, ui, log),
        editor(vfs, project.root(), ui) {
    runtime.load_scripts();
    runtime.start("start");
    REQUIRE(ui.is_shown("menu"));
  }

  static vnmaki::Vfs make_vfs(const TempProject& project) {
    vnmaki::Vfs result;
    REQUIRE(result.mount(vnmaki::make_directory_mount(project.root())));
    return result;
  }

  // Selects the node, sets the fields (key and value), saves. Returns the problem.
  std::string change(const std::string& screen, SourcePath path,
                     const std::vector<std::pair<std::string, std::string>>& values) {
    REQUIRE(editor.select(screen, std::move(path)));
    REQUIRE(editor.problem().empty());
    for (const auto& [key, value] : values) {
      field(key).value = value;
    }
    return editor.save();
  }

  vnmaki::ScreenEditor::Field& field(const std::string& key) {
    for (vnmaki::ScreenEditor::Field& candidate : editor.fields()) {
      if (candidate.key == key) {
        return candidate;
      }
    }
    FAIL("no field " << key);
    return editor.fields().front();
  }

  TempProject project;
  vnmaki::Vfs vfs;
  vnmaki::Scene scene;
  vnmaki::JobSystem jobs{0};
  vnmaki::LogBuffer log;
  vnmaki::UiHost ui;
  vnmaki::ScriptRuntime runtime;
  vnmaki::HotReload hot;
  vnmaki::ScreenEditor editor;
};

std::string bytes_of(const std::string& source, const vnmaki::ScanField& field) {
  return source.substr(field.begin, field.end - field.begin);
}

const vnmaki::ScanField* find_field(const vnmaki::ScanNode& node, std::string_view key) {
  for (const vnmaki::ScanField& field : node.fields) {
    if (field.key == key) {
      return &field;
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("script scan: tokens know their bytes") {
  const std::string source = "local x = \"a\\\"b\" .. [[long]] -5 --c\nfoo 'q'";
  const std::vector<vnmaki::LuaToken> tokens = vnmaki::tokenize_lua(source);
  std::vector<std::string> written;
  for (const vnmaki::LuaToken& token : tokens) {
    written.push_back(source.substr(token.begin, token.end - token.begin));
  }
  const std::vector<std::string> expected{"local",    "x", "=", "\"a\\\"b\"", ".",  ".",
                                          "[[long]]", "-", "5", "foo",        "'q'"};
  CHECK(written == expected);
}

TEST_CASE("screen editor: the scan records each literal of a screen table with its span") {
  const std::string source = read_bytes(kFixtures / "project" / kUi);
  const std::vector<vnmaki::ScanScreenTree> screens = vnmaki::scan_screen_trees(source);
  REQUIRE(screens.size() == 4);
  CHECK(screens[0].name == "menu");
  CHECK(screens[3].name == "fn");
  CHECK_FALSE(screens[3].root.has_value());  // a function: its nodes depend on `args`

  REQUIRE(screens[0].root.has_value());
  const vnmaki::ScanNode& vbox = *screens[0].root;
  CHECK(vbox.kind == "vbox");
  CHECK_FALSE(vbox.dynamic);
  REQUIRE(vbox.children.size() == 4);
  const vnmaki::ScanField* gap = find_field(vbox, "gap");
  REQUIRE(gap != nullptr);
  CHECK(gap->kind == ScanValueKind::kNumber);
  CHECK(bytes_of(source, *gap) == "12");
  const vnmaki::ScanField* align = find_field(vbox, "align");
  REQUIRE(align != nullptr);
  CHECK(align->kind == ScanValueKind::kString);
  CHECK(align->text == "start");
  CHECK(bytes_of(source, *align) == "\"start\"");

  const vnmaki::ScanNode& hello = vbox.children[0];
  const vnmaki::ScanField* hello_text = find_field(hello, "");
  REQUIRE(hello_text != nullptr);
  CHECK(hello_text->text == "Hello \"there\"");  // decoded
  CHECK(bytes_of(source, *hello_text) == "\"Hello \\\"there\\\"\"");

  const vnmaki::ScanNode& button = vbox.children[1];
  CHECK(button.kind == "button");
  CHECK(bytes_of(source, *find_field(button, "")) == "'Start'");
  CHECK(find_field(button, "disabled")->kind == ScanValueKind::kBoolean);
  CHECK(find_field(button, "returns")->kind == ScanValueKind::kString);

  const vnmaki::ScanNode& hbox = vbox.children[2];
  const vnmaki::ScanField* x = find_field(hbox, "x");
  CHECK(x->kind == ScanValueKind::kNumber);
  CHECK(bytes_of(source, *x) == "-6");  // the minus sign is part of the literal
  CHECK(find_field(hbox, "padding")->kind == ScanValueKind::kComputed);
  CHECK(bytes_of(source, *find_field(hbox, "padding")) == "pad");
  REQUIRE(hbox.children.size() == 2);
  CHECK(find_field(hbox.children[0], "size")->kind == ScanValueKind::kComputed);
  CHECK(bytes_of(source, *find_field(hbox.children[0], "size")) == "10 + 20");
  CHECK(hbox.children[1].kind == "bar");

  const vnmaki::ScanNode& short_form = vbox.children[3];
  CHECK(short_form.kind == "text");
  CHECK(bytes_of(source, *find_field(short_form, "")) == "\"short form\"");

  CHECK(screens[1].root->kind == "grid");
  CHECK(screens[1].root->children.size() == 3);
  CHECK(screens[2].root->dynamic);  // `rows()` is a computed child
}

TEST_CASE("screen editor: a later field of the same name makes the earlier one computed") {
  const std::string source = "screen \"s\" { box { gap = 1, gap = 2 } }";
  const std::vector<vnmaki::ScanScreenTree> screens = vnmaki::scan_screen_trees(source);
  REQUIRE(screens.size() == 1);
  const vnmaki::ScanNode& box = *screens[0].root;
  REQUIRE(box.fields.size() == 2);
  CHECK(box.fields[0].kind == ScanValueKind::kComputed);
  CHECK(box.fields[1].kind == ScanValueKind::kNumber);
}

TEST_CASE("screen editor: a string literal that is written reads back the same") {
  for (const char quote : {'"', '\''}) {
    for (int byte = 1; byte < kByteValues; ++byte) {
      const std::string content =
          std::string{"a"} + static_cast<char>(byte) + std::string{"1\\x\n"};
      const std::string literal = vnmaki::lua_string_literal(content, quote);
      const std::vector<vnmaki::LuaToken> tokens = vnmaki::tokenize_lua(literal);
      REQUIRE(tokens.size() == 1);
      CHECK(tokens[0].kind == vnmaki::LuaTokenKind::kString);
      CHECK(tokens[0].text == content);
      CHECK(tokens[0].end == literal.size());
    }
  }
}

TEST_CASE("screen editor: a value must fit the kind of the field") {
  using vnmaki::field_literal;
  CHECK(field_literal(ScanValueKind::kNumber, "-1.5", '"').problem.empty());
  CHECK(field_literal(ScanValueKind::kNumber, "2e3", '"').text == "2e3");
  for (const char* bad : {"", "abc", "1x", "0x10", "+1", " 1", "inf", "nan", "1e999"}) {
    CHECK_FALSE(field_literal(ScanValueKind::kNumber, bad, '"').problem.empty());
  }
  CHECK(field_literal(ScanValueKind::kBoolean, "true", '"').text == "true");
  CHECK_FALSE(field_literal(ScanValueKind::kBoolean, "yes", '"').problem.empty());
  CHECK(field_literal(ScanValueKind::kString, "a\"b", '\'').text == "'a\"b'");
  CHECK_FALSE(field_literal(ScanValueKind::kComputed, "1", '"').problem.empty());
}

TEST_CASE("screen editor: rewrite_spans changes the spans and nothing else, or refuses") {
  const std::string source = "ab -- c\r\nde = 12 ;";
  const auto edit = [&](std::size_t begin, std::size_t end, std::string expected,
                        std::string replacement) {
    return vnmaki::SpanEdit{
        .begin = begin, .end = end, .expected = std::move(expected), .replacement = replacement};
  };
  const vnmaki::RewriteResult done = vnmaki::rewrite_spans(
      source, {edit(14, 16, "12", "7"), edit(0, 2, "ab", "xyz")});  // out of order is fine
  CHECK(done.problem.empty());
  CHECK(done.text == "xyz -- c\r\nde = 7 ;");
  CHECK_FALSE(vnmaki::rewrite_spans(source, {edit(14, 16, "13", "7")}).problem.empty());
  CHECK_FALSE(vnmaki::rewrite_spans(source, {edit(0, 2, "ab", "x"), edit(1, 3, "b ", "y")})
                  .problem.empty());
  CHECK_FALSE(vnmaki::rewrite_spans(source, {edit(17, 99, "", "x")}).problem.empty());
}

TEST_CASE("screen editor: a save replaces only the bytes of the edited literals") {
  Game game{"rewrite"};
  const std::string before = read_bytes(game.project.ui());
  const std::string expected = read_bytes(kFixtures / "expected" / kUi);
  REQUIRE(before != expected);
  REQUIRE(before.find("\r\n") != std::string::npos);

  // Two literals of one table in one save, then one node after another.
  CHECK(game.change("menu", {}, {{"gap", "20"}, {"align", "center"}}).empty());
  CHECK(game.change("menu", {0}, {{"", std::string{kNewHello}}, {"size", "31.5"}}).empty());
  CHECK(game.change("menu", {1}, {{"", "Go"}, {"disabled", "true"}}).empty());
  CHECK(game.change("menu", {2}, {{"x", "4"}}).empty());
  CHECK(game.change("menu", {2, 0}, {{"", std::string{kNewSound}}}).empty());
  CHECK(game.change("menu", {2, 1}, {{"value", "7.25"}}).empty());

  CHECK(read_bytes(game.project.ui()) == expected);
  for (const fs::directory_entry& entry : fs::directory_iterator{game.project.root() / "scripts"}) {
    CHECK(entry.path().extension() == ".lua");  // the temporary file is gone
  }
  CHECK(game.editor.problem().empty());  // the editor still maps the node
}

TEST_CASE("screen editor: a computed field is read-only and a save leaves the file alone") {
  Game game{"computed"};
  const std::string before = read_bytes(game.project.ui());
  REQUIRE(game.editor.select("menu", {2}));
  vnmaki::ScreenEditor::Field& padding = game.field("padding");
  CHECK(padding.kind == ScanValueKind::kComputed);
  CHECK_FALSE(padding.editable);
  CHECK(padding.original == "pad");  // the source text of the expression
  padding.value = "99";
  CHECK_FALSE(padding.changed());
  CHECK(game.editor.save().empty());  // nothing to write
  CHECK(read_bytes(game.project.ui()) == before);

  REQUIRE(game.editor.select("menu", {2, 0}));
  CHECK_FALSE(game.field("size").editable);
  CHECK(game.field("").editable);  // the literal text of the same node is
}

TEST_CASE("screen editor: a value of the wrong kind is refused and the file stays") {
  Game game{"invalid"};
  const std::string before = read_bytes(game.project.ui());
  CHECK(game.change("menu", {}, {{"gap", "wide"}}).find("number") != std::string::npos);
  CHECK(game.change("menu", {1}, {{"disabled", "maybe"}}).find("true") != std::string::npos);
  CHECK(read_bytes(game.project.ui()) == before);
}

TEST_CASE("screen editor: a file that changed after the pick is not written") {
  Game game{"stale"};
  REQUIRE(game.editor.select("menu", {}));
  game.field("gap").value = "20";
  std::string changed = read_bytes(game.project.ui());
  const std::size_t at = changed.find("gap=12");
  REQUIRE(at != std::string::npos);
  changed.replace(at, 6, "gap=13");  // somebody else edited the literal
  write_bytes(game.project.ui(), changed);
  CHECK(game.editor.save().find("changed") != std::string::npos);
  CHECK(read_bytes(game.project.ui()) == changed);
}

TEST_CASE("screen editor: a node maps back to its table, also in a grid") {
  Game game{"mapping"};
  REQUIRE(game.editor.select("menu", {2, 1}));
  CHECK(game.editor.problem().empty());
  CHECK(game.editor.source_kind() == "bar");
  CHECK(game.editor.location() == "scripts/ui.lua:11");
  const UiNode* bar = game.editor.node();
  REQUIRE(bar != nullptr);
  CHECK(bar->kind == vnmaki::NodeKind::kBar);
  CHECK(game.ui.find("menu")->children[2].children[1].rect == bar->rect);

  // The cells of a grid are in rows that the engine adds; they map to the written order.
  REQUIRE(game.editor.select("cells", {2}));
  CHECK(game.editor.problem().empty());
  CHECK(game.editor.node()->text == "c");
  CHECK(game.field("").original == "c");
  REQUIRE(game.editor.select("cells", {1}));
  CHECK(game.field("").original == "b");
  REQUIRE(game.editor.select("cells", {}));
  CHECK(game.editor.source_kind() == "grid");
  CHECK(game.field("columns").original == "2");

  CHECK_FALSE(game.editor.select("menu", {9}));  // no such node
  CHECK_FALSE(game.editor.select("nothing", {}));
  CHECK_FALSE(game.editor.has_selection());
}

TEST_CASE("screen editor: a node with no sure source is read-only") {
  Game game{"readonly"};
  // A table with a computed child: the table's own literals are editable, its children are not.
  REQUIRE(game.editor.select("dyn", {}));
  CHECK(game.editor.problem().empty());
  CHECK(game.field("gap").editable);
  REQUIRE(game.editor.select("dyn", {1}));
  CHECK_FALSE(game.editor.problem().empty());
  CHECK(game.editor.fields().empty());
  CHECK(game.editor.save().find("read-only") == 0);
  // A function: its nodes depend on `args`.
  REQUIRE(game.editor.select("fn", {}));
  CHECK_FALSE(game.editor.problem().empty());
  // A screen that the scripts do not define.
  game.ui.show("extra", vnmaki::make_node(vnmaki::NodeKind::kBox), false);
  REQUIRE(game.editor.select("extra", {}));
  CHECK_FALSE(game.editor.problem().empty());

  // The file has a node more than the screen shows (it was edited, and no reload ran yet).
  std::string source = read_bytes(game.project.ui());
  const std::size_t at = source.find("    text \"short form\"");
  REQUIRE(at != std::string::npos);
  source.insert(at, "    text { \"new\" },\r\n");
  write_bytes(game.project.ui(), source);
  REQUIRE(game.editor.select("menu", {0}));
  CHECK_FALSE(game.editor.problem().empty());
  CHECK(game.editor.problem().find("nodes") != std::string::npos);
}

TEST_CASE("screen editor: a click picks the node on top, and nothing outside") {
  Game game{"pick"};
  for (const char* name : {"cells", "dyn", "fn"}) {
    REQUIRE(game.ui.hide(name));
  }
  const auto center = [](const UiNode& node) {
    return vnmaki::PointF{.x = node.rect.x + (node.rect.width / 2),
                          .y = node.rect.y + (node.rect.height / 2)};
  };
  const UiNode* menu = game.ui.find("menu");
  REQUIRE(menu != nullptr);
  const UiNode& bar = menu->children[2].children[1];
  const std::optional<vnmaki::PickedNode> picked = vnmaki::pick_node(game.ui, center(bar));
  REQUIRE(picked.has_value());
  CHECK(picked->screen == "menu");
  CHECK(picked->path == SourcePath{2, 1});

  CHECK(vnmaki::pick_node(game.ui, center(menu->children[0]))->path == SourcePath{0});
  CHECK(vnmaki::pick_node(game.ui, {-kOne, -kOne}) == std::nullopt);

  CHECK(game.editor.select_at(center(menu->children[1])));
  CHECK(game.editor.source_kind() == "button");
  CHECK_FALSE(game.editor.select_at({-kOne, -kOne}));
  CHECK_FALSE(game.editor.has_selection());
}

TEST_CASE("screen editor: after a save, hot reload shows the change") {
  Game game{"reload"};
  REQUIRE(game.ui.find("menu")->style.gap == 12);
  CHECK(game.change("menu", {}, {{"gap", "20"}}).empty());
  CHECK(game.change("menu", {0}, {{"", std::string{kNewHello}}}).empty());
  CHECK(game.ui.find("menu")->style.gap == 12);  // not yet: the host has the old tree

  const std::vector<std::string> batch{std::string{kUi}};
  game.hot.apply(batch);
  CHECK(game.hot.banner().empty());
  const UiNode* menu = game.ui.find("menu");
  REQUIRE(menu != nullptr);
  CHECK(menu->style.gap == 20);
  CHECK(menu->children[0].text == kNewHello);

  // The selection follows the new tree.
  REQUIRE(game.editor.select("menu", {0}));
  CHECK(game.field("").original == kNewHello);
}
