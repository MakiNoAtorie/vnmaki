// Vertical layout (docs/SPEC.md section 13) with the box font of tools/generators/text_fixtures.py.
// The font has vertical metrics (an upright glyph advances one em down and is centred on its
// column) but no `vert` alternates, so "。" and brackets keep their horizontal shapes; a real font
// adds its vertical forms (in the top right of the cell) on top of this. At 20 pixels per em a
// column is 20 pixels thick with the box's ascent 16, and the advances down are: kana and kanji
// 20, Latin 10 (turned 90 degrees clockwise).

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "engine/dialogue/dialogue_presenter.hpp"
#include "engine/l10n/translator.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/runtime/project_config.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/text/markup.hpp"
#include "engine/text/text.hpp"
#include "engine/ui/ui_build.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/ui/ui_layout.hpp"
#include "engine/ui/ui_node.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::kNoWrap;
using vnmaki::PointF;
using vnmaki::TextLayout;
using vnmaki::TextOrientation;

constexpr float kSize = 20;

vnmaki::Font box_font() {
  vnmaki::Vfs vfs;
  REQUIRE(
      vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_FIXTURES_DIR} / "text")));
  return vnmaki::load_font(vfs, "fonts/boxes.ttf");
}

TextLayout vertical(const vnmaki::Font& font, std::string_view markup,
                    float column_length = kNoWrap) {
  const vnmaki::FontSet fonts{.base = {.regular = &font}};
  return layout_text(fonts, vnmaki::parse_markup(markup), kSize, column_length,
                     TextOrientation::kVertical);
}

std::vector<PointF> origins(const vnmaki::TextLine& column) {
  std::vector<PointF> found;
  found.reserve(column.glyphs.size());
  for (const vnmaki::LayoutGlyph& glyph : column.glyphs) {
    found.push_back(glyph.origin);
  }
  return found;
}

/// The part of `text` in each column.
std::vector<std::string> columns_of(std::string_view text, float column_length) {
  const vnmaki::Font font = box_font();
  const TextLayout layout = vertical(font, text, column_length);
  std::vector<std::string> columns;
  for (std::size_t i = 0; i < layout.lines.size(); ++i) {
    const std::size_t begin = layout.lines.at(i).glyphs.at(0).text_offset;
    const std::size_t end =
        i + 1 < layout.lines.size() ? layout.lines.at(i + 1).glyphs.at(0).text_offset : text.size();
    columns.emplace_back(text.substr(begin, end - begin));
  }
  return columns;
}

}  // namespace

TEST_CASE("vertical text: upright glyphs run down a column, centred on it") {
  const vnmaki::Font font = box_font();
  const TextLayout layout = vertical(font, "あいう");
  REQUIRE(layout.vertical);
  REQUIRE(layout.lines.size() == 1);
  const vnmaki::TextLine& column = layout.lines[0];
  // The origin is the glyph's left end on the baseline: the em box is 20 wide and 20 below its
  // top, whose baseline is 16 down.
  CHECK(origins(column) == std::vector<PointF>{{.x = 0, .y = 16}, {.x = 0, .y = 36}, {.x = 0, .y = 56}});
  CHECK(column.width == 60);
  CHECK(column.top == 0);
  CHECK(column.height == 20);
  CHECK(column.baseline == 10);
  CHECK_FALSE(column.glyphs[0].sideways);
  CHECK(vnmaki::layout_extent(layout) == PointF{.x = 20, .y = 60});
}

TEST_CASE("vertical text: columns run from right to left") {
  const vnmaki::Font font = box_font();
  const TextLayout layout = vertical(font, "あいうえお", 40);
  REQUIRE(layout.lines.size() == 3);
  CHECK(layout.lines[0].top == 40);
  CHECK(layout.lines[1].top == 20);
  CHECK(layout.lines[2].top == 0);
  CHECK(layout.lines[0].glyphs[0].origin == PointF{.x = 40, .y = 16});
  CHECK(layout.lines[1].glyphs[0].origin == PointF{.x = 20, .y = 16});
  CHECK(layout.lines[2].glyphs[0].origin == PointF{.x = 0, .y = 16});
  CHECK(layout.lines[2].glyphs.size() == 1);
  CHECK(vnmaki::layout_extent(layout) == PointF{.x = 60, .y = 40});
  // A new paragraph starts a new column, also at the left of the first.
  const TextLayout paragraphs = vertical(font, "あ\nい");
  REQUIRE(paragraphs.lines.size() == 2);
  CHECK(paragraphs.lines[0].glyphs[0].origin.x == 20);
  CHECK(paragraphs.lines[1].glyphs[0].origin.x == 0);
}

TEST_CASE("vertical text: a Latin run is turned 90 degrees clockwise") {
  const vnmaki::Font font = box_font();
  const TextLayout layout = vertical(font, "あabい");
  REQUIRE(layout.lines.size() == 1);
  const vnmaki::TextLine& column = layout.lines[0];
  REQUIRE(column.glyphs.size() == 4);
  CHECK_FALSE(column.glyphs[0].sideways);
  CHECK(column.glyphs[1].sideways);
  CHECK(column.glyphs[2].sideways);
  CHECK_FALSE(column.glyphs[3].sideways);
  // The em box of a turned glyph is centred on the column: its baseline is 6 pixels left of the
  // centre line (the ascent 16 less half the line height). The advance is the letter's own.
  CHECK(column.glyphs[1].origin == PointF{.x = 4, .y = 20});
  CHECK(column.glyphs[2].origin == PointF{.x = 4, .y = 30});
  CHECK(column.glyphs[3].origin == PointF{.x = 0, .y = 56});
  CHECK(column.width == 60);

  // The quad of a turned glyph is the bitmap turned about the glyph's origin: the box glyph "a"
  // is 8 wide and 10 high, 1 right of its origin.
  vnmaki::GlyphAtlas atlas;
  CHECK(atlas.prepare(layout));
  const std::vector<vnmaki::Quad> quads = vnmaki::text_quads(layout, atlas, {}, 2);
  REQUIRE(quads.size() == 2);
  const auto& turned = quads[1].corners;
  CHECK(turned[0] == PointF{.x = 14, .y = 21});
  CHECK(turned[1] == PointF{.x = 14, .y = 29});
  CHECK(turned[2] == PointF{.x = 4, .y = 29});
  CHECK(turned[3] == PointF{.x = 4, .y = 21});
}

TEST_CASE("vertical text: a column breaks at its length, and kinsoku applies to it") {
  // Without the rules the fifth character would end the first column.
  CHECK(columns_of("あいうえきゃ", 80) == std::vector<std::string>{"あいうえ", "きゃ"});
  CHECK(columns_of("あいうえカーか", 80) == std::vector<std::string>{"あいうえ", "カーか"});
  // A closing mark does not start a column: the character before it goes with it.
  CHECK(columns_of("あいうえお」か", 80) == std::vector<std::string>{"あいうえ", "お」か"});
  CHECK(columns_of("あいうえお。か", 80) == std::vector<std::string>{"あいうえ", "お。か"});
  // An opening bracket does not end a column.
  CHECK(columns_of("あいうえ「か", 80) == std::vector<std::string>{"あいうえ", "「か"});
  // Latin words break at spaces, by the width of the turned letters.
  CHECK(columns_of("ab cd ef", 50) == std::vector<std::string>{"ab cd ", "ef"});
}

TEST_CASE("vertical text: ruby goes to the right of its column, centred on its base") {
  const vnmaki::Font font = box_font();
  const TextLayout layout = vertical(font, "{ruby=にほん}日本{/ruby}語");
  // The reveal counts only the base text.
  REQUIRE(layout.glyph_count == 3);
  REQUIRE(layout.lines.size() == 1);
  const vnmaki::TextLine& column = layout.lines[0];
  // 10 pixels of ruby column right of the 20 of the base column.
  CHECK(column.height == 30);
  CHECK(column.baseline == 10);
  CHECK(origins(column) == std::vector<PointF>{{.x = 0, .y = 16}, {.x = 0, .y = 36}, {.x = 0, .y = 56}});
  REQUIRE(column.ruby.size() == 1);
  const vnmaki::RubyText& ruby = column.ruby[0];
  CHECK(ruby.base_glyph == 0);
  REQUIRE(ruby.glyphs.size() == 3);
  // Three kana of 10 pixels (30 long) centred on the 40 pixels of their base: 5 down.
  CHECK(ruby.glyphs[0].pixel_size == 10);
  CHECK(ruby.glyphs[0].origin == PointF{.x = 20, .y = 13});
  CHECK(ruby.glyphs[1].origin == PointF{.x = 20, .y = 23});
  CHECK(ruby.glyphs[2].origin == PointF{.x = 20, .y = 33});
  CHECK(vnmaki::layout_extent(layout) == PointF{.x = 30, .y = 60});

  // Two columns, only the first with ruby (30 thick): the ruby stays on the right of it.
  const TextLayout two = vertical(font, "あ{ruby=に}い{/ruby}う", 40);
  REQUIRE(two.lines.size() == 2);
  CHECK(two.lines[0].top == 20);
  CHECK(two.lines[0].glyphs[0].origin.x == 20);
  CHECK(two.lines[0].ruby.at(0).glyphs.at(0).origin.x == 40);
  CHECK(two.lines[1].top == 0);
}

TEST_CASE("vertical text: a link has a region on its column, and the reveal counts glyphs") {
  const vnmaki::Font font = box_font();
  const TextLayout layout = vertical(font, "あ{link=go}いう{/link}え", 60);
  REQUIRE(layout.lines.size() == 2);
  REQUIRE(layout.links.size() == 1);
  CHECK(layout.links[0].id == "go");
  // The first column is 20 wide at the right edge (x 20..40); the link covers its last two glyphs.
  CHECK(layout.links[0].rect == vnmaki::RectF{.x = 20, .y = 20, .width = 20, .height = 40});
  CHECK(vnmaki::link_at(layout, {.x = 30, .y = 30}) == "go");
  CHECK(vnmaki::link_at(layout, {.x = 30, .y = 10}).empty());
  CHECK(vnmaki::revealed_glyphs(layout, 10, 0.25) == 2);
}

namespace {

constexpr vnmaki::Size kCanvas{.width = 300, .height = 200};
constexpr float kTateTextSize = 30;
constexpr float kColumnLimit = 70;
constexpr float kColumnWidth = 30;

const vnmaki::UiNode* find_text(const vnmaki::UiNode& node, std::string_view text) {
  if (node.text == text) {
    return &node;
  }
  for (const vnmaki::UiNode& child : node.children) {
    if (const vnmaki::UiNode* found = find_text(child, text)) {
      return found;
    }
  }
  return nullptr;
}

vnmaki::UiNode built_screen(vnmaki::ScriptRuntime& runtime, const std::string& name,
                            const vnmaki::ScreenNodeDef& args) {
  vnmaki::ScreenNodeDef tree;
  REQUIRE(runtime.build_screen(name, args, tree).empty());
  return vnmaki::build_ui_tree(tree);
}

}  // namespace

TEST_CASE("vertical text: project.toml sets it for a language") {
  const vnmaki::ProjectConfig config = vnmaki::parse_project_config(
      "title = \"T\"\nlanguages = [\"en\", \"ja\"]\n[vertical]\nja = true\nen = false\n");
  CHECK(config.vertical.at("ja"));
  CHECK_FALSE(config.vertical.at("en"));
  const auto fails = [](std::string_view text) {
    return vnmaki::parse_project_config("title = \"T\"\nlanguages = [\"en\"]\n[vertical]\n" +
                                        std::string{text});
  };
  CHECK_THROWS_WITH_AS(fails("en = \"yes\"\n"),
                       doctest::Contains("'vertical.en' must be true or false"),
                       vnmaki::ProjectConfigError);
  CHECK_THROWS_WITH_AS(fails("ja = true\n"), doctest::Contains("'vertical' has language 'ja'"),
                       vnmaki::ProjectConfigError);

  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs(
      {{"project.toml", "title = \"T\"\nlanguages = [\"en\", \"ja\"]\n[vertical]\nja = true\n"}});
  vnmaki::Translator translator{vfs, vnmaki::load_project_config(vfs)};
  CHECK_FALSE(translator.vertical());
  REQUIRE(translator.set_language("ja").empty());
  CHECK(translator.vertical());
  REQUIRE(translator.set_language("en").empty());
  CHECK_FALSE(translator.vertical());
}

TEST_CASE("vertical text: the say and NVL screens are vertical when the language says so") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs(
      {{"scripts/main.lua", "label('start', function() end)\n"},
       {"styles/tate.lua", "style('say_text', { size = 30, vertical = true })\n"}});
  vnmaki::UiHost host{kCanvas};
  vnmaki::ScriptRuntime runtime{vfs, {.screens = &host}};
  runtime.load_scripts();
  const vnmaki::ScreenNodeDef say_args{.kind = "args",
                                       .props = {{"speaker", "Eileen"}, {"text", "Hi"}}};
  const vnmaki::NvlPage page{{.speaker = "Eileen", .text = "one"}, {.speaker = "", .text = "two"}};
  const auto say = [&] { return built_screen(runtime, "say", say_args); };
  const auto nvl = [&] { return built_screen(runtime, "nvl", vnmaki::make_nvl_args(page, true)); };

  // Horizontal by default: no height on the line.
  const vnmaki::UiNode horizontal = say();
  REQUIRE(find_text(horizontal, "Hi") != nullptr);
  CHECK_FALSE(find_text(horizontal, "Hi")->style.vertical);
  CHECK_FALSE(find_text(horizontal, "Hi")->style.height.has_value());
  CHECK_FALSE(find_text(nvl(), "two")->style.vertical);

  // project.toml: the flag makes the line vertical, wrapping at a height inside the box.
  REQUIRE(runtime.set_language_styles("", true).empty());
  const vnmaki::UiNode tall = say();
  const vnmaki::UiNode* line = find_text(tall, "Hi");
  REQUIRE(line != nullptr);
  CHECK(line->style.vertical);
  REQUIRE(line->style.height.has_value());
  CHECK(*line->style.height > 0);
  CHECK(*line->style.height < tall.children.at(0).style.height.value());
  CHECK_FALSE(find_text(tall, "Eileen")->style.vertical);  // the name stays horizontal
  // NVL: every line is a column, the newest first (at the left), and the name of a line is a
  // column after it (at its right).
  const vnmaki::UiNode page_screen = nvl();
  const vnmaki::UiNode* two = find_text(page_screen, "two");
  const vnmaki::UiNode* one = find_text(page_screen, "one");
  const vnmaki::UiNode* name = find_text(page_screen, "Eileen");
  REQUIRE((two != nullptr && one != nullptr && name != nullptr));
  CHECK(two->style.vertical);
  CHECK(two->reveal);
  CHECK(one->style.vertical);
  CHECK(name->style.vertical);
  const std::vector<vnmaki::UiNode>& row = page_screen.children.at(0).children;
  REQUIRE(row.size() == 3);
  CHECK(row[0].text == "two");
  CHECK(row[1].text == "one");
  CHECK(row[2].text == "Eileen");

  // Back to horizontal, then the same through a style file, with no project.toml flag.
  REQUIRE(runtime.set_language_styles("", false).empty());
  CHECK_FALSE(find_text(say(), "Hi")->style.vertical);
  REQUIRE(runtime.set_language_styles("styles/tate.lua", false).empty());
  CHECK(find_text(say(), "Hi")->style.vertical);
  CHECK(find_text(say(), "Hi")->style.text_size == kTateTextSize);
  CHECK_FALSE(find_text(nvl(), "two")->style.vertical);  // the file set only say_text
}

TEST_CASE("vertical text: only text nodes take it, and layout wraps them at their height") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({{"scripts/main.lua", R"(
style("tate", { vertical = true, size = 20 })
screen("s", function()
  return hbox { text { "ab", style = "tate", height = 70 }, button { "Go", style = "tate" } }
end)
label("start", function() end)
)"}});
  vnmaki::UiHost host{kCanvas};
  vnmaki::ScriptRuntime runtime{vfs, {.screens = &host}};
  runtime.load_scripts();
  vnmaki::UiNode screen = built_screen(runtime, "s", {});  // the button leaves `vertical` out
  REQUIRE(screen.children.size() == 2);
  CHECK(screen.children[0].style.vertical);
  CHECK_FALSE(screen.children[1].style.vertical);

  std::vector<float> limits;
  vnmaki::layout_tree(screen, kCanvas, [&](const vnmaki::UiNode& node, float limit) {
    if (node.style.vertical) {
      limits.push_back(limit);
    }
    return vnmaki::MeasuredSize{.width = kColumnWidth, .height = kColumnLimit + 20};
  });
  // The text wraps at its height, and the width of its box is that of its columns.
  CHECK(limits == std::vector<float>{kColumnLimit});
  CHECK(screen.children[0].rect.width == kColumnWidth);
  CHECK(screen.children[0].rect.height == kColumnLimit);
}
