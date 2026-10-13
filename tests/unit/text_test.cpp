// The font comes from tools/generators/text_fixtures.py: every glyph is a box, 1000 units per
// em, so at 20 pixels per em a Latin letter advances 10 pixels, a kana or kanji 20, a line is 20
// pixels high and its baseline is 16 pixels down. The numbers below follow from that.

#include "engine/text/text.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "engine/platform/geometry.hpp"
#include "engine/render/image.hpp"
#include "engine/render/renderer.hpp"
#include "engine/vfs/vfs.hpp"

namespace {

using vnmaki::kNoWrap;
using vnmaki::layout_text;
using vnmaki::PointF;

constexpr float kSize = 20;
constexpr std::string_view kFontPath = "fonts/boxes.ttf";

vnmaki::Vfs fixture_vfs() {
  vnmaki::Vfs vfs;
  REQUIRE(
      vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_FIXTURES_DIR} / "text")));
  return vfs;
}

vnmaki::Font boxes_font() { return vnmaki::load_font(fixture_vfs(), kFontPath); }

/// The part of `text` each line holds, from where the line's glyphs start to where the next
/// line's start. Every line must have a glyph.
std::vector<std::string> line_texts(const vnmaki::TextLayout& layout, std::string_view text) {
  std::vector<std::string> lines;
  lines.reserve(layout.lines.size());
  for (std::size_t i = 0; i < layout.lines.size(); ++i) {
    const std::size_t begin = layout.lines.at(i).glyphs.at(0).text_offset;
    const std::size_t end =
        i + 1 < layout.lines.size() ? layout.lines.at(i + 1).glyphs.at(0).text_offset : text.size();
    lines.emplace_back(text.substr(begin, end - begin));
  }
  return lines;
}

std::vector<std::string> wrapped(std::string_view text, float wrap_width) {
  const vnmaki::Font font = boxes_font();
  return line_texts(layout_text(font, text, kSize, wrap_width), text);
}

/// The RGBA bytes of pixel (x, y).
std::vector<int> pixel_at(const vnmaki::Image& image, int x, int y) {
  constexpr std::size_t kChannels = 4;
  const std::size_t offset =
      ((static_cast<std::size_t>(y) * static_cast<std::size_t>(image.size.width)) +
       static_cast<std::size_t>(x)) *
      kChannels;
  return {image.pixels.at(offset), image.pixels.at(offset + 1), image.pixels.at(offset + 2),
          image.pixels.at(offset + 3)};
}

const std::vector<int> kSolid{255, 255, 255, 255};
const std::vector<int> kClear{0, 0, 0, 0};

}  // namespace

TEST_CASE("text: a font loads through the VFS") {
  const vnmaki::Vfs vfs = fixture_vfs();
  CHECK_NOTHROW((void)vnmaki::load_font(vfs, kFontPath));
  CHECK_THROWS_AS((void)vnmaki::load_font(vfs, "fonts/missing.ttf"), vnmaki::FontError);
  CHECK_THROWS_AS((void)vnmaki::load_font(vfs, "fonts/not_a_font.ttf"), vnmaki::FontError);
  CHECK_THROWS_AS((void)vnmaki::load_font(vfs, "fonts/truncated.ttf"), vnmaki::FontError);
  CHECK_THROWS_AS((void)vnmaki::load_font(vfs, "fonts/empty.ttf"), vnmaki::FontError);
}

TEST_CASE("text: one line puts each glyph an advance after the last, on the baseline") {
  const vnmaki::Font font = boxes_font();
  const vnmaki::TextLayout layout = layout_text(font, "Hi a", kSize, kNoWrap);

  CHECK(layout.pixel_size == 20);
  CHECK(layout.line_height == 20);
  CHECK(layout.glyph_count == 4);
  REQUIRE(layout.lines.size() == 1);
  const vnmaki::TextLine& line = layout.lines.front();
  CHECK(line.baseline == 16);
  CHECK(line.width == 40);
  REQUIRE(line.glyphs.size() == 4);
  CHECK(line.glyphs[0].origin == PointF{.x = 0, .y = 16});
  CHECK(line.glyphs[1].origin == PointF{.x = 10, .y = 16});
  CHECK(line.glyphs[2].origin == PointF{.x = 20, .y = 16});
  CHECK(line.glyphs[3].origin == PointF{.x = 30, .y = 16});
  CHECK(line.glyphs[3].text_offset == 3);
  // The font numbers its glyphs in code point order after .notdef: space is glyph 1.
  CHECK(line.glyphs[2].glyph == 1);
}

TEST_CASE("text: layout scales with the pixel size") {
  const vnmaki::Font font = boxes_font();
  const vnmaki::TextLayout layout = layout_text(font, "ab", 2 * kSize, kNoWrap);

  CHECK(layout.line_height == 40);
  REQUIRE(layout.lines.size() == 1);
  CHECK(layout.lines[0].glyphs.at(1).origin == PointF{.x = 20, .y = 32});
}

TEST_CASE("text: a character the font lacks becomes the missing-character glyph") {
  const vnmaki::Font font = boxes_font();
  const vnmaki::TextLayout layout = layout_text(font, "~", kSize, kNoWrap);

  REQUIRE(layout.glyph_count == 1);
  CHECK(layout.lines.at(0).glyphs.at(0).glyph == 0);
}

TEST_CASE("text: Latin text wraps at spaces, and the space at the line end hangs") {
  // "aaa bbb" is exactly 70 pixels wide; the space after it does not count.
  CHECK(wrapped("aaa bbb ccc", 70) == std::vector<std::string>{"aaa bbb ", "ccc"});
  CHECK(wrapped("aaa bbb ccc", 69) == std::vector<std::string>{"aaa ", "bbb ", "ccc"});
  CHECK(wrapped("aaa bbb ccc", kNoWrap) == std::vector<std::string>{"aaa bbb ccc"});
}

TEST_CASE("text: wrapped lines start at the left, one line height apart") {
  const vnmaki::Font font = boxes_font();
  const vnmaki::TextLayout layout = layout_text(font, "aaa bbb ccc", kSize, 70);

  REQUIRE(layout.lines.size() == 2);
  CHECK(layout.glyph_count == 11);
  CHECK(layout.lines[0].width == 70);
  CHECK(layout.lines[1].width == 30);
  CHECK(layout.lines[1].baseline == 36);
  CHECK(layout.lines[1].glyphs.at(0).origin == PointF{.x = 0, .y = 36});
  CHECK(layout.lines[1].glyphs.at(0).text_offset == 8);
}

TEST_CASE("text: a word wider than the line splits between characters") {
  CHECK(wrapped("abcdefgh", 30) == std::vector<std::string>{"abc", "def", "gh"});
  CHECK(wrapped("ab abcdefgh", 30) == std::vector<std::string>{"ab ", "abc", "def", "gh"});
}

TEST_CASE("text: Japanese text wraps between characters, without spaces") {
  CHECK(wrapped("あいうえお", 40) == std::vector<std::string>{"あい", "うえ", "お"});
  CHECK(wrapped("日本語のテキスト", 60) == std::vector<std::string>{"日本語", "のテキ", "スト"});
}

TEST_CASE("text: Japanese kinsoku rules keep closing marks off the line start") {
  // Three characters fit a line, but "。" and "、" may not start one, so "う" moves down with it.
  CHECK(wrapped("あいう。えお", 60) == std::vector<std::string>{"あい", "う。え", "お"});
  CHECK(wrapped("あいう、えお", 60) == std::vector<std::string>{"あい", "う、え", "お"});
  // An opening bracket may not end a line.
  CHECK(wrapped("あい「う」", 60) == std::vector<std::string>{"あい", "「う」"});
}

TEST_CASE("text: mixed Latin and Japanese text wraps in one paragraph") {
  CHECK(wrapped("Hello, VN. こんにちは、世界。", 100) ==
        std::vector<std::string>{"Hello, VN. ", "こんにち", "は、世界。"});
}

TEST_CASE("text: line ends in the text always break") {
  const vnmaki::Font font = boxes_font();
  const vnmaki::TextLayout layout = layout_text(font, "ab\n\ncd\r\ne\n", kSize, kNoWrap);

  REQUIRE(layout.lines.size() == 4);
  CHECK(layout.glyph_count == 5);
  CHECK(layout.lines[0].glyphs.size() == 2);
  CHECK(layout.lines[1].glyphs.empty());
  CHECK(layout.lines[1].baseline == 36);
  CHECK(layout.lines[2].glyphs.size() == 2);
  CHECK(layout.lines[2].glyphs.at(0).text_offset == 4);
  CHECK(layout.lines[3].glyphs.size() == 1);
  CHECK(layout.lines[3].baseline == 76);
}

TEST_CASE("text: empty text has no lines") {
  const vnmaki::Font font = boxes_font();
  const vnmaki::TextLayout layout = layout_text(font, "", kSize, kNoWrap);

  CHECK(layout.lines.empty());
  CHECK(layout.glyph_count == 0);
}

TEST_CASE("text: layout rejects a pixel size outside the limits") {
  const vnmaki::Font font = boxes_font();
  CHECK_THROWS_AS((void)layout_text(font, "a", 0, kNoWrap), std::invalid_argument);
  CHECK_THROWS_AS((void)layout_text(font, "a", -kSize, kNoWrap), std::invalid_argument);
  CHECK_THROWS_AS((void)layout_text(font, "a", 2 * vnmaki::kMaxFontPixelSize, kNoWrap),
                  std::invalid_argument);
}

TEST_CASE("text: a typewriter reveal shows glyphs at its speed in characters per second") {
  using vnmaki::revealed_glyphs;
  CHECK(revealed_glyphs(10, 20.0, 0.0) == 0);
  CHECK(revealed_glyphs(10, 20.0, 0.25) == 5);
  CHECK(revealed_glyphs(10, 20.0, 0.29) == 5);
  CHECK(revealed_glyphs(10, 20.0, 0.5) == 10);
  CHECK(revealed_glyphs(10, 20.0, 60.0) == 10);
  CHECK(revealed_glyphs(10, 20.0, -1.0) == 0);
  // No speed means no typewriter: everything shows at once.
  CHECK(revealed_glyphs(10, 0.0, 0.0) == 10);
}

TEST_CASE("text: the glyph atlas holds each glyph's bitmap at the layout's pixel size") {
  const vnmaki::Font font = boxes_font();
  vnmaki::GlyphAtlas atlas{font};
  CHECK(atlas.pixel_size() == 0);
  CHECK(atlas.find(0) == nullptr);

  const vnmaki::TextLayout layout = layout_text(font, "Ab. あ", kSize, kNoWrap);
  CHECK(atlas.prepare(layout));
  CHECK(atlas.pixel_size() == 20);

  const auto entry = [&](std::size_t index) {
    const vnmaki::AtlasGlyph* found = atlas.find(layout.lines.at(0).glyphs.at(index).glyph);
    REQUIRE(found != nullptr);
    return *found;
  };
  // "A": a box from (50, 0) to (450, 700) font units, 50 units to the pixel.
  const vnmaki::AtlasGlyph upper = entry(0);
  CHECK(upper.width == 8);
  CHECK(upper.height == 14);
  CHECK(upper.left == 1);
  CHECK(upper.top == 14);
  const vnmaki::AtlasGlyph lower = entry(1);
  CHECK(lower.width == 8);
  CHECK(lower.height == 10);
  CHECK(lower.top == 10);
  const vnmaki::AtlasGlyph dot = entry(2);
  CHECK(dot.width == 4);
  CHECK(dot.height == 4);
  CHECK(dot.left == 3);
  CHECK(dot.top == 4);
  // A space has an entry without pixels.
  CHECK(entry(3).width == 0);
  // "あ": a box from (50, -100) to (950, 800).
  const vnmaki::AtlasGlyph wide = entry(4);
  CHECK(wide.width == 18);
  CHECK(wide.height == 18);
  CHECK(wide.left == 1);
  CHECK(wide.top == 16);

  // Each bitmap is solid white inside its rectangle, with a transparent border around it.
  const vnmaki::Image& image = atlas.image();
  REQUIRE(image.pixels.size() == static_cast<std::size_t>(image.size.width) *
                                     static_cast<std::size_t>(image.size.height) * 4);
  for (const vnmaki::AtlasGlyph& glyph : {upper, lower, dot, wide}) {
    CAPTURE(glyph.width);
    CHECK(pixel_at(image, glyph.x, glyph.y) == kSolid);
    CHECK(pixel_at(image, glyph.x + glyph.width - 1, glyph.y + glyph.height - 1) == kSolid);
    CHECK(pixel_at(image, glyph.x - 1, glyph.y) == kClear);
    CHECK(pixel_at(image, glyph.x, glyph.y - 1) == kClear);
    CHECK(pixel_at(image, glyph.x + glyph.width, glyph.y) == kClear);
    CHECK(pixel_at(image, glyph.x, glyph.y + glyph.height) == kClear);
  }
}

TEST_CASE("text: the glyph atlas changes only for new glyphs or a new pixel size") {
  const vnmaki::Font font = boxes_font();
  vnmaki::GlyphAtlas atlas{font};
  const vnmaki::TextLayout layout = layout_text(font, "Ab", kSize, kNoWrap);
  const std::uint32_t upper = layout.lines.at(0).glyphs.at(0).glyph;

  CHECK(atlas.prepare(layout));
  CHECK_FALSE(atlas.prepare(layout));
  CHECK_FALSE(atlas.prepare(layout_text(font, "bA", kSize, kNoWrap)));
  CHECK(atlas.prepare(layout_text(font, "c", kSize, kNoWrap)));
  // Glyphs of earlier layouts stay.
  REQUIRE(atlas.find(upper) != nullptr);
  CHECK(atlas.find(upper)->width == 8);

  // The window doubled its pixels (a resize, or a move to a high-DPI display): the atlas is
  // rebuilt at the size the glyphs now have on screen.
  constexpr float kScale = 2;
  CHECK(atlas.prepare(layout, kScale));
  CHECK(atlas.pixel_size() == 40);
  REQUIRE(atlas.find(upper) != nullptr);
  CHECK(atlas.find(upper)->width == 16);
  CHECK(atlas.find(upper)->height == 28);
  CHECK(atlas.find(upper)->left == 2);
  CHECK(atlas.find(upper)->top == 28);
  CHECK_FALSE(atlas.prepare(layout, kScale));
  CHECK(atlas.prepare(layout));
  CHECK(atlas.find(upper)->width == 8);
}

TEST_CASE("text: the glyph atlas rejects a pixel size outside the limits") {
  const vnmaki::Font font = boxes_font();
  vnmaki::GlyphAtlas atlas{font};
  const vnmaki::TextLayout layout = layout_text(font, "A", vnmaki::kMaxFontPixelSize, kNoWrap);
  CHECK_THROWS_AS((void)atlas.prepare(layout, 2), std::invalid_argument);
  CHECK_THROWS_AS((void)atlas.prepare(layout, 0), std::invalid_argument);
}

TEST_CASE("text: quads place the first glyphs' bitmaps on whole pixels") {
  const vnmaki::Font font = boxes_font();
  vnmaki::GlyphAtlas atlas{font};
  const vnmaki::TextLayout layout = layout_text(font, "A b", kSize, kNoWrap);
  REQUIRE(atlas.prepare(layout));
  constexpr PointF kOrigin{.x = 4, .y = 2};
  const vnmaki::Color tint = vnmaki::rgba8(255, 224, 128);

  const std::vector<vnmaki::Quad> all =
      vnmaki::text_quads(layout, atlas, kOrigin, layout.glyph_count, tint);
  // The space draws nothing.
  REQUIRE(all.size() == 2);
  // "A": pen at x 0, box 1 pixel right of it, 14 pixels up from the baseline at y 16.
  CHECK(all[0].corners[0] == PointF{.x = 5, .y = 4});
  CHECK(all[0].corners[2] == PointF{.x = 13, .y = 18});
  // "b": pen at x 20, 10 pixels high.
  CHECK(all[1].corners[0] == PointF{.x = 25, .y = 8});
  CHECK(all[1].corners[2] == PointF{.x = 33, .y = 18});
  CHECK(all[1].tint == tint);

  const vnmaki::AtlasGlyph* upper = atlas.find(layout.lines.at(0).glyphs.at(0).glyph);
  REQUIRE(upper != nullptr);
  const auto width = static_cast<float>(atlas.image().size.width);
  const auto height = static_cast<float>(atlas.image().size.height);
  CHECK(all[0].uv.x == doctest::Approx(static_cast<float>(upper->x) / width));
  CHECK(all[0].uv.y == doctest::Approx(static_cast<float>(upper->y) / height));
  CHECK(all[0].uv.width == doctest::Approx(8 / width));
  CHECK(all[0].uv.height == doctest::Approx(14 / height));

  // A reveal draws the first N glyphs; the space counts as one.
  CHECK(vnmaki::text_quads(layout, atlas, kOrigin, 0).empty());
  CHECK(vnmaki::text_quads(layout, atlas, kOrigin, 2).size() == 1);
  CHECK(vnmaki::text_quads(layout, atlas, kOrigin, 3).size() == 2);
}

TEST_CASE("text: quads follow the atlas's scale, so text is drawn in screen pixels") {
  const vnmaki::Font font = boxes_font();
  vnmaki::GlyphAtlas atlas{font};
  const vnmaki::TextLayout layout = layout_text(font, "ab", kSize, kNoWrap);
  constexpr float kScale = 2;
  REQUIRE(atlas.prepare(layout, kScale));

  const std::vector<vnmaki::Quad> quads =
      vnmaki::text_quads(layout, atlas, PointF{}, layout.glyph_count);
  REQUIRE(quads.size() == 2);
  // "b" at twice the size: pen at x 20, box 2 pixels right, 20 pixels up from the baseline at 32.
  CHECK(quads[1].corners[0] == PointF{.x = 22, .y = 12});
  CHECK(quads[1].corners[2] == PointF{.x = 38, .y = 32});
}
