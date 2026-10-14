// Layout of text with tags, more than one font and more than one direction. The fonts come from
// tools/generators/text_fixtures.py. At 20 pixels per em a line is 20 pixels high with its
// baseline 16 down, and the advances are: Latin and Hebrew 10 (bold 12, italic 8), kana and kanji
// 20, Greek (only in fallback.ttf) 15.

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "engine/platform/geometry.hpp"
#include "engine/render/image.hpp"
#include "engine/render/renderer.hpp"
#include "engine/text/markup.hpp"
#include "engine/text/text.hpp"
#include "engine/vfs/vfs.hpp"

namespace {

using vnmaki::kNoWrap;
using vnmaki::layout_text;
using vnmaki::parse_markup;
using vnmaki::PointF;
using vnmaki::TextLayout;
using vnmaki::TextLine;

constexpr float kSize = 20;

vnmaki::Font fixture_font(std::string_view name) {
  vnmaki::Vfs vfs;
  REQUIRE(
      vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_FIXTURES_DIR} / "text")));
  return vnmaki::load_font(vfs, "fonts/" + std::string{name});
}

/// The fixture fonts, and a FontSet that uses all of them.
struct Fonts {
  vnmaki::Font regular = fixture_font("boxes.ttf");
  vnmaki::Font bold = fixture_font("boxes_bold.ttf");
  vnmaki::Font italic = fixture_font("boxes_italic.ttf");
  vnmaki::Font greek = fixture_font("fallback.ttf");

  [[nodiscard]] vnmaki::FontSet set() const {
    vnmaki::FontSet fonts;
    fonts.base = {.regular = &regular, .bold = &bold, .italic = &italic, .bold_italic = nullptr};
    fonts.families["wide"] = {
        .regular = &bold, .bold = nullptr, .italic = nullptr, .bold_italic = nullptr};
    fonts.fallbacks = {&greek};
    return fonts;
  }

  [[nodiscard]] TextLayout layout(std::string_view markup, float wrap_width = kNoWrap) const {
    return layout_text(set(), parse_markup(markup), kSize, wrap_width);
  }
};

/// The x of each glyph's origin on line `line`, in logical order.
std::vector<float> origins_x(const TextLayout& layout, std::size_t line = 0) {
  std::vector<float> xs;
  xs.reserve(layout.lines.at(line).glyphs.size());
  for (const vnmaki::LayoutGlyph& glyph : layout.lines.at(line).glyphs) {
    xs.push_back(glyph.origin.x);
  }
  return xs;
}

/// The left edge of each quad.
std::vector<float> lefts(const std::vector<vnmaki::Quad>& quads) {
  std::vector<float> xs;
  xs.reserve(quads.size());
  for (const vnmaki::Quad& quad : quads) {
    xs.push_back(quad.corners[0].x);
  }
  return xs;
}

/// The part of `text` on each line.
std::vector<std::string> wrapped(std::string_view text, float wrap_width) {
  const vnmaki::Font font = fixture_font("boxes.ttf");
  const TextLayout layout = layout_text(font, text, kSize, wrap_width);
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

}  // namespace

TEST_CASE("styled text: text without tags lays out as with one font") {
  const Fonts fonts;
  const TextLayout styled = fonts.layout("aaa bbb ccc", 70);
  CHECK(styled == layout_text(fonts.regular, "aaa bbb ccc", kSize, 70));
  REQUIRE(styled.lines.size() == 2);
  CHECK(styled.lines[0].top == 0);
  CHECK(styled.lines[0].height == 20);
  CHECK(styled.lines[1].top == 20);
  const vnmaki::LayoutGlyph& glyph = styled.lines[1].glyphs.at(1);
  CHECK(glyph.pixel_size == 20);
  CHECK(glyph.advance == 10);
  CHECK_FALSE(glyph.rtl);
  CHECK_FALSE(glyph.color.has_value());
  REQUIRE(styled.fonts.size() == 1);
  CHECK(styled.fonts[0] == &fonts.regular);
}

TEST_CASE("styled text: color gives its glyphs their colour in place of the tint") {
  const Fonts fonts;
  const TextLayout layout = fonts.layout("a{color=#f00}b{/color}c");
  REQUIRE(layout.glyph_count == 3);
  const std::vector<vnmaki::LayoutGlyph>& glyphs = layout.lines.at(0).glyphs;
  CHECK_FALSE(glyphs[0].color.has_value());
  CHECK(glyphs[1].color == vnmaki::rgba8(255, 0, 0));
  CHECK_FALSE(glyphs[2].color.has_value());

  vnmaki::GlyphAtlas atlas;
  CHECK(atlas.prepare(layout));
  const vnmaki::Color tint = vnmaki::rgba8(0, 0, 255);
  const std::vector<vnmaki::Quad> quads = vnmaki::text_quads(layout, atlas, {}, 3, tint);
  REQUIRE(quads.size() == 3);
  CHECK(quads[0].tint == tint);
  CHECK(quads[1].tint == vnmaki::rgba8(255, 0, 0));
  CHECK(quads[2].tint == tint);
}

TEST_CASE("styled text: size scales its glyphs, and the line makes room for them") {
  const Fonts fonts;
  const TextLayout layout = fonts.layout("a{size=40}b{/size}c\nd");
  REQUIRE(layout.lines.size() == 2);
  const TextLine& line = layout.lines[0];
  // The large glyph needs 32 pixels above the baseline and 8 below.
  CHECK(line.height == 40);
  CHECK(line.baseline == 32);
  CHECK(origins_x(layout) == std::vector<float>{0, 10, 30});
  CHECK(line.glyphs.at(0).origin.y == 32);
  CHECK(line.glyphs.at(1).pixel_size == 40);
  CHECK(line.glyphs.at(1).advance == 20);
  CHECK(line.width == 40);
  CHECK(layout.lines[1].top == 40);
  CHECK(layout.lines[1].baseline == 56);
  CHECK(layout.line_height == 20);

  // The atlas holds the same glyph shape at both sizes ("b" and "c" are the same box).
  vnmaki::GlyphAtlas atlas;
  CHECK(atlas.prepare(layout));
  const vnmaki::AtlasGlyph* large = atlas.find(layout, line.glyphs.at(1));
  const vnmaki::AtlasGlyph* small = atlas.find(layout, line.glyphs.at(2));
  REQUIRE(large != nullptr);
  REQUIRE(small != nullptr);
  CHECK(large->width == 16);
  CHECK(large->height == 20);
  CHECK(small->width == 8);
  CHECK(small->height == 10);
}

TEST_CASE("styled text: font selects a named family") {
  const Fonts fonts;
  const TextLayout layout = fonts.layout("{font=wide}a{/font}a{font=nothing}a{/font}a");
  // "wide" is the bold stand-in, 12 pixels to the letter. An unknown family is the base one.
  CHECK(origins_x(layout) == std::vector<float>{0, 12, 22, 32});
  const std::vector<vnmaki::LayoutGlyph>& glyphs = layout.lines.at(0).glyphs;
  CHECK(layout.fonts.at(glyphs[0].font) == &fonts.bold);
  CHECK(layout.fonts.at(glyphs[1].font) == &fonts.regular);
  CHECK(layout.fonts.at(glyphs[2].font) == &fonts.regular);
}

TEST_CASE("styled text: b and i select the family's bold and italic fonts") {
  const Fonts fonts;
  const TextLayout layout = fonts.layout("a{b}a{/b}{i}a{/i}a{b}{i}a{/i}{/b}a");
  // Regular 10, bold 12, italic 8, regular 10, then bold italic: the family has no such font, so
  // the bold one.
  CHECK(origins_x(layout) == std::vector<float>{0, 10, 22, 30, 40, 52});
  const std::vector<vnmaki::LayoutGlyph>& glyphs = layout.lines.at(0).glyphs;
  CHECK(layout.fonts.at(glyphs[1].font) == &fonts.bold);
  CHECK(layout.fonts.at(glyphs[2].font) == &fonts.italic);
  CHECK(layout.fonts.at(glyphs[4].font) == &fonts.bold);
}

TEST_CASE("styled text: a right-to-left line runs from the right, in logical glyph order") {
  const Fonts fonts;
  // Three Hebrew letters, two bytes each.
  const TextLayout layout = fonts.layout("אבג");
  REQUIRE(layout.glyph_count == 3);
  const std::vector<vnmaki::LayoutGlyph>& glyphs = layout.lines.at(0).glyphs;
  CHECK(glyphs[0].text_offset == 0);
  CHECK(glyphs[1].text_offset == 2);
  CHECK(glyphs[2].text_offset == 4);
  CHECK(glyphs[0].rtl);
  // The first letter is the rightmost one.
  CHECK(origins_x(layout) == std::vector<float>{20, 10, 0});
  CHECK(layout.lines[0].width == 30);

  // The reveal shows the first letter first: the box at the right (1 pixel of left bearing).
  vnmaki::GlyphAtlas atlas;
  CHECK(atlas.prepare(layout));
  CHECK(lefts(vnmaki::text_quads(layout, atlas, {}, 1)) == std::vector<float>{21});
  CHECK(lefts(vnmaki::text_quads(layout, atlas, {}, 2)) == std::vector<float>{21, 11});

  // The caret is at the leading (right) edge of its letter, and ends at the left.
  CHECK(vnmaki::caret_position(layout, 0) == PointF{.x = 30, .y = 16});
  CHECK(vnmaki::caret_position(layout, 2) == PointF{.x = 20, .y = 16});
  CHECK(vnmaki::caret_position(layout, 6) == PointF{.x = 0, .y = 16});
}

TEST_CASE("styled text: Hebrew in a Latin line is reversed in place") {
  const Fonts fonts;
  // Bytes: a 0, b 1, space 2, alef 3, bet 5, space 7, c 8, d 9.
  const TextLayout layout = fonts.layout("ab אב cd");
  REQUIRE(layout.glyph_count == 8);
  const std::vector<vnmaki::LayoutGlyph>& glyphs = layout.lines.at(0).glyphs;
  // Logical order: the offsets rise.
  CHECK(glyphs[3].text_offset == 3);
  CHECK(glyphs[4].text_offset == 5);
  CHECK(glyphs[3].rtl);
  CHECK_FALSE(glyphs[5].rtl);
  // Visual order: bet (the second Hebrew letter) is left of alef.
  CHECK(origins_x(layout) == std::vector<float>{0, 10, 20, 40, 30, 50, 60, 70});

  // Four glyphs revealed: a, b, the space (no quad) and alef, which leaves a gap where bet goes.
  vnmaki::GlyphAtlas atlas;
  CHECK(atlas.prepare(layout));
  CHECK(lefts(vnmaki::text_quads(layout, atlas, {}, 4)) == std::vector<float>{1, 11, 41});

  CHECK(vnmaki::caret_position(layout, 1) == PointF{.x = 10, .y = 16});
  CHECK(vnmaki::caret_position(layout, 3) == PointF{.x = 50, .y = 16});
  CHECK(vnmaki::caret_position(layout, 5) == PointF{.x = 40, .y = 16});
  CHECK(vnmaki::caret_position(layout, 8) == PointF{.x = 60, .y = 16});
  CHECK(vnmaki::caret_position(layout, 10) == PointF{.x = 80, .y = 16});
}

TEST_CASE("styled text: a right-to-left paragraph wraps in logical order") {
  const Fonts fonts;
  // Two Hebrew words; the second goes to the next line.
  const TextLayout layout = fonts.layout("אבג דה", 35);
  REQUIRE(layout.lines.size() == 2);
  CHECK(layout.lines[0].glyphs.at(0).text_offset == 0);
  CHECK(layout.lines[1].glyphs.at(0).text_offset == 7);
  // The space at the end of the first line hangs to the left of the text.
  CHECK(origins_x(layout, 0) == std::vector<float>{20, 10, 0, -10});
  CHECK(origins_x(layout, 1) == std::vector<float>{10, 0});
}

TEST_CASE("styled text: kinsoku keeps small kana and long vowel marks off the line start") {
  // Without the rule the fifth character would end the first line.
  CHECK(wrapped("あいうえきゃ", 100) == std::vector<std::string>{"あいうえ", "きゃ"});
  CHECK(wrapped("あいうえカーか", 100) == std::vector<std::string>{"あいうえ", "カーか"});
  CHECK(wrapped("あいうえおっか", 100) == std::vector<std::string>{"あいうえ", "おっか"});
  // An opening bracket does not end a line, and a closing one does not start one.
  CHECK(wrapped("あいうえ「か", 100) == std::vector<std::string>{"あいうえ", "「か"});
  CHECK(wrapped("あいうえお」か", 100) == std::vector<std::string>{"あいうえ", "お」か"});
}

TEST_CASE("styled text: a character the font lacks comes from the fallback chain") {
  const Fonts fonts;
  // Beta is only in the fallback font, where it is 15 pixels wide.
  const TextLayout layout = fonts.layout("aβa");
  CHECK(origins_x(layout) == std::vector<float>{0, 10, 25});
  const std::vector<vnmaki::LayoutGlyph>& glyphs = layout.lines.at(0).glyphs;
  CHECK(layout.fonts.at(glyphs[0].font) == &fonts.regular);
  CHECK(layout.fonts.at(glyphs[1].font) == &fonts.greek);
  CHECK(glyphs[1].glyph != 0);
  CHECK(layout.fonts.at(glyphs[2].font) == &fonts.regular);

  // The atlas holds glyphs of both fonts: a Greek box is 650 units (13 pixels) wide.
  vnmaki::GlyphAtlas atlas;
  CHECK(atlas.prepare(layout));
  REQUIRE(atlas.find(layout, glyphs[1]) != nullptr);
  CHECK(atlas.find(layout, glyphs[1])->width == 13);
  REQUIRE(atlas.find(layout, glyphs[0]) != nullptr);
  CHECK(atlas.find(layout, glyphs[0])->width == 8);
  CHECK(vnmaki::text_quads(layout, atlas, {}, 3).size() == 3);

  // Without a chain, and for a character no font has, it is the missing-character glyph.
  vnmaki::FontSet no_chain = fonts.set();
  no_chain.fallbacks.clear();
  const TextLayout missing = layout_text(no_chain, parse_markup("aβa"), kSize, kNoWrap);
  CHECK(missing.lines.at(0).glyphs.at(1).glyph == 0);
  CHECK(missing.fonts.size() == 1);
  CHECK(fonts.layout("aЖa").lines.at(0).glyphs.at(1).glyph == 0);
}

TEST_CASE("styled text: ruby text is half size, centred above its base") {
  const Fonts fonts;
  const TextLayout layout = fonts.layout("{ruby=にほん}日本{/ruby}語");
  // The reveal counts only the base text.
  REQUIRE(layout.glyph_count == 3);
  REQUIRE(layout.lines.size() == 1);
  const TextLine& line = layout.lines[0];
  // 10 pixels of ruby line above the 20 of the base line.
  CHECK(line.height == 30);
  CHECK(line.baseline == 26);
  CHECK(origins_x(layout) == std::vector<float>{0, 20, 40});
  REQUIRE(line.ruby.size() == 1);
  const vnmaki::RubyText& ruby = line.ruby[0];
  CHECK(ruby.base_glyph == 0);
  REQUIRE(ruby.glyphs.size() == 3);
  // Three kana of 10 pixels over a base 40 pixels wide.
  CHECK(ruby.glyphs[0].pixel_size == 10);
  CHECK(ruby.glyphs[0].origin == PointF{.x = 5, .y = 8});
  CHECK(ruby.glyphs[1].origin == PointF{.x = 15, .y = 8});
  CHECK(ruby.glyphs[2].origin == PointF{.x = 25, .y = 8});

  // The ruby shows as soon as its first base glyph does.
  vnmaki::GlyphAtlas atlas;
  CHECK(atlas.prepare(layout));
  CHECK(vnmaki::text_quads(layout, atlas, {}, 0).empty());
  CHECK(vnmaki::text_quads(layout, atlas, {}, 1).size() == 4);
  CHECK(vnmaki::text_quads(layout, atlas, {}, 3).size() == 6);
}

TEST_CASE("styled text: a base narrower than its ruby gets space on both sides") {
  const Fonts fonts;
  // Four kana of 10 pixels over one Latin letter of 10: 15 pixels of space on each side.
  const TextLayout layout = fonts.layout("a{ruby=にほんご}b{/ruby}c");
  CHECK(origins_x(layout) == std::vector<float>{0, 25, 50});
  REQUIRE(layout.lines.at(0).ruby.size() == 1);
  CHECK(layout.lines[0].ruby[0].base_glyph == 1);
  CHECK(layout.lines[0].ruby[0].glyphs.at(0).origin.x == 10);
  CHECK(layout.lines[0].width == 60);
}

TEST_CASE("styled text: a line does not break in a ruby base") {
  const Fonts fonts;
  const TextLayout layout = fonts.layout("あい{ruby=a}うえ{/ruby}お", 60);
  REQUIRE(layout.lines.size() == 2);
  CHECK(layout.lines[0].glyphs.size() == 2);
  CHECK(layout.lines[1].glyphs.size() == 3);
  CHECK(layout.lines[0].ruby.empty());
  CHECK(layout.lines[1].ruby.size() == 1);
}

TEST_CASE("styled text: a link has a hit region on each of its lines") {
  const Fonts fonts;
  const TextLayout layout = fonts.layout("go {link=shop}to it{/link} now", 60);
  // "go to " / "it now": the link is the last three glyphs of line 1 and the first two of line 2.
  REQUIRE(layout.lines.size() == 2);
  REQUIRE(layout.links.size() == 2);
  CHECK(layout.links[0] ==
        vnmaki::LinkRegion{.id = "shop", .rect = {.x = 30, .y = 0, .width = 30, .height = 20}});
  CHECK(layout.links[1] ==
        vnmaki::LinkRegion{.id = "shop", .rect = {.x = 0, .y = 20, .width = 20, .height = 20}});

  CHECK(vnmaki::link_at(layout, {.x = 35, .y = 5}) == "shop");
  CHECK(vnmaki::link_at(layout, {.x = 5, .y = 25}) == "shop");
  CHECK(vnmaki::link_at(layout, {.x = 5, .y = 5}).empty());
  CHECK(vnmaki::link_at(layout, {.x = 35, .y = 25}).empty());
  CHECK(vnmaki::link_at(layout, {.x = 35, .y = 45}).empty());
}

TEST_CASE("styled text: speed and pause change when the reveal shows each glyph") {
  const Fonts fonts;
  // At 10 characters per second: a at 0.1 s and b at 0.2 s; a pause of 1 s; c at 1.3 s and d at
  // 1.4 s; e and f at 2 characters per second, at 1.9 s and 2.4 s; g at 2.5 s.
  const TextLayout layout = fonts.layout("ab{pause=1}cd{speed=2}ef{/speed}g");
  constexpr double kSpeed = 10;
  constexpr double kAfterB = 0.25;
  constexpr double kInPause = 1.25;
  constexpr double kAfterC = 1.35;
  constexpr double kAfterD = 1.45;
  constexpr double kBeforeE = 1.85;
  constexpr double kAfterE = 1.95;
  constexpr double kAfterF = 2.45;
  constexpr double kAfterG = 2.55;
  using vnmaki::revealed_glyphs;
  CHECK(revealed_glyphs(layout, kSpeed, 0.0) == 0);
  CHECK(revealed_glyphs(layout, kSpeed, kAfterB) == 2);
  CHECK(revealed_glyphs(layout, kSpeed, kInPause) == 2);
  CHECK(revealed_glyphs(layout, kSpeed, kAfterC) == 3);
  CHECK(revealed_glyphs(layout, kSpeed, kAfterD) == 4);
  CHECK(revealed_glyphs(layout, kSpeed, kBeforeE) == 4);
  CHECK(revealed_glyphs(layout, kSpeed, kAfterE) == 5);
  CHECK(revealed_glyphs(layout, kSpeed, kAfterF) == 6);
  CHECK(revealed_glyphs(layout, kSpeed, kAfterG) == 7);
  // A reveal without a speed shows everything at once, pauses included.
  CHECK(revealed_glyphs(layout, 0.0, 0.0) == 7);

  // Without tags it is the plain reveal.
  const TextLayout plain = fonts.layout("abcdefg");
  constexpr double kHalfSecond = 0.5;
  CHECK(revealed_glyphs(plain, kSpeed, kHalfSecond) == 5);
  CHECK(revealed_glyphs(plain, kSpeed, kAfterG) == 7);
}

TEST_CASE("styled text: layout needs a base font and sizes inside the limits") {
  const Fonts fonts;
  CHECK_THROWS_AS((void)layout_text(vnmaki::FontSet{}, parse_markup("a"), kSize, kNoWrap),
                  std::invalid_argument);
  vnmaki::TextStyle huge;
  huge.size = vnmaki::kMaxFontPixelSize * 2;
  CHECK_THROWS_AS((void)layout_text(fonts.set(), vnmaki::plain_text("a", huge), kSize, kNoWrap),
                  std::invalid_argument);
}

TEST_CASE("styled text: a distance atlas holds the distance to the glyph's edge") {
  const Fonts fonts;
  const TextLayout layout = fonts.layout("A");
  vnmaki::GlyphAtlas atlas{vnmaki::GlyphRaster::kDistance};
  CHECK(atlas.prepare(layout));
  const vnmaki::AtlasGlyph* found = atlas.find(layout, layout.lines.at(0).glyphs.at(0));
  REQUIRE(found != nullptr);
  // "A" is a box of 8 by 14 pixels, 1 pixel right of its origin. The field reaches 8 pixels
  // (kSdfSpread) out on each side.
  const vnmaki::AtlasGlyph glyph = *found;
  CHECK(glyph.width == 24);
  CHECK(glyph.height == 30);
  CHECK(glyph.left == -7);
  CHECK(glyph.top == 22);

  // The value at a pixel is 128 on the edge and 16 for each pixel of distance: more inside,
  // less outside. A pixel's centre is half a pixel from its sides.
  const vnmaki::Image& image = atlas.image();
  constexpr std::size_t kChannels = 4;
  const auto value = [&](int x, int y) {
    const std::size_t offset =
        ((static_cast<std::size_t>(glyph.y + y) * static_cast<std::size_t>(image.size.width)) +
         static_cast<std::size_t>(glyph.x + x)) *
        kChannels;
    CHECK(image.pixels.at(offset) == image.pixels.at(offset + 3));
    return int{image.pixels.at(offset + 3)};
  };
  // Half a pixel inside and outside the left edge.
  CHECK(value(8, 15) == 136);
  CHECK(value(7, 15) == 120);
  // Three and a half pixels inside (the box's middle column) and outside.
  CHECK(value(11, 15) == 184);
  CHECK(value(4, 15) == 72);
  // Half a pixel outside two edges: the distance to the corner, 0.707 pixels.
  CHECK(value(7, 7) == 117);
  // Farther than the field reaches.
  CHECK(value(0, 0) == 0);
}
