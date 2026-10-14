// Fuzzes font loading (engine/text/text.hpp): the input is a font file, which is then used the
// way the engine uses one: text is laid out with it and its glyphs go into an atlas. Fonts ship
// with games and mods, so they are untrusted input.
//
// Besides "does not crash": bad data is reported only as FontError; layout is deterministic and
// self-consistent; wrapping only splits lines (same glyphs, same order, as the unwrapped text);
// atlas bitmaps are within the size limits, lie inside the image with a border, do not overlap,
// and the image is white premultiplied by coverage.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

#include "engine/render/image.hpp"
#include "engine/render/renderer.hpp"
#include "engine/text/markup.hpp"
#include "engine/text/text.hpp"

namespace {

// Two paragraphs: Latin with spaces, and Japanese with marks the kinsoku rules hold back.
constexpr std::string_view kText = "Fuzz, a VN font.\nこんにちは、世界。";
constexpr std::size_t kParagraphs = 2;
constexpr std::string_view kMarkup =
    "{b}Fuzz{/b}, {size=18}a{/size} {ruby=かな}世界{/ruby} אבג {link=x}font{/link}.";
constexpr float kWrapWidth = 90;
constexpr std::array<float, 2> kPixelSizes{12, 33.5F};

void require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

std::vector<vnmaki::LayoutGlyph> all_glyphs(const vnmaki::TextLayout& layout) {
  std::vector<vnmaki::LayoutGlyph> glyphs;
  glyphs.reserve(layout.glyph_count);
  for (const vnmaki::TextLine& line : layout.lines) {
    glyphs.insert(glyphs.end(), line.glyphs.begin(), line.glyphs.end());
  }
  return glyphs;
}

void check_layout(const vnmaki::TextLayout& layout) {
  const std::vector<vnmaki::LayoutGlyph> glyphs = all_glyphs(layout);
  require(glyphs.size() == layout.glyph_count);
  require(layout.lines.size() >= kParagraphs);
  std::uint32_t previous_offset = 0;
  for (const vnmaki::LayoutGlyph& glyph : glyphs) {
    require(glyph.text_offset < kText.size());
    require(glyph.text_offset >= previous_offset);
    previous_offset = glyph.text_offset;
    require(std::isfinite(glyph.origin.x) && std::isfinite(glyph.origin.y));
  }
}

/// Wrapping must only move glyphs to other lines.
void check_same_glyphs(const vnmaki::TextLayout& wrapped, const vnmaki::TextLayout& unwrapped) {
  const std::vector<vnmaki::LayoutGlyph> a = all_glyphs(wrapped);
  const std::vector<vnmaki::LayoutGlyph> b = all_glyphs(unwrapped);
  require(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    require(a[i].glyph == b[i].glyph && a[i].text_offset == b[i].text_offset);
  }
}

bool overlap(const vnmaki::AtlasGlyph& a, const vnmaki::AtlasGlyph& b) {
  return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

void check_atlas(const vnmaki::GlyphAtlas& atlas, const vnmaki::TextLayout& layout) {
  const vnmaki::Image& image = atlas.image();
  const int width = image.size.width;
  const int height = image.size.height;
  require(width > 0 && height > 0);
  require(width <= vnmaki::kMaxImageDimension && height <= vnmaki::kMaxImageDimension);
  constexpr std::size_t kChannels = 4;
  require(image.pixels.size() ==
          static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * kChannels);
  for (std::size_t offset = 0; offset < image.pixels.size(); offset += kChannels) {
    const std::uint8_t alpha = image.pixels[offset + 3];
    require(image.pixels[offset] == alpha && image.pixels[offset + 1] == alpha &&
            image.pixels[offset + 2] == alpha);
  }

  // A bitmap may be a pixel wider than the outline on each side.
  const int max_extent = static_cast<int>(std::ceil(atlas.pixel_size())) * vnmaki::kMaxGlyphEms + 2;
  std::vector<vnmaki::AtlasGlyph> placed;
  placed.reserve(layout.glyph_count);
  for (const vnmaki::LayoutGlyph& glyph : all_glyphs(layout)) {
    const vnmaki::AtlasGlyph* entry = atlas.find(glyph.glyph);
    require(entry != nullptr);
    require(entry->width >= 0 && entry->height >= 0);
    require((entry->width == 0) == (entry->height == 0));
    if (entry->width == 0) {
      continue;
    }
    require(entry->width <= max_extent && entry->height <= max_extent);
    require(entry->x >= 1 && entry->y >= 1);
    require(entry->x + entry->width < width && entry->y + entry->height < height);
    for (const vnmaki::AtlasGlyph& other : placed) {
      require(other == *entry || !overlap(other, *entry));
    }
    placed.push_back(*entry);
  }
}

void check_quads(const vnmaki::TextLayout& layout, const vnmaki::GlyphAtlas& atlas) {
  const std::size_t visible = layout.glyph_count / 2;
  const std::vector<vnmaki::Quad> quads =
      vnmaki::text_quads(layout, atlas, vnmaki::PointF{}, visible);
  require(quads.size() <= visible);
  for (const vnmaki::Quad& quad : quads) {
    for (const vnmaki::PointF& corner : quad.corners) {
      require(std::isfinite(corner.x) && std::isfinite(corner.y));
    }
    require(quad.uv.x >= 0 && quad.uv.y >= 0 && quad.uv.x + quad.uv.width <= 1 &&
            quad.uv.y + quad.uv.height <= 1);
  }
}

void check_font(const vnmaki::Font& font, float pixel_size) {
  const vnmaki::TextLayout unwrapped =
      vnmaki::layout_text(font, kText, pixel_size, vnmaki::kNoWrap);
  check_layout(unwrapped);
  require(unwrapped.lines.size() == kParagraphs);
  const vnmaki::TextLayout wrapped = vnmaki::layout_text(font, kText, pixel_size, kWrapWidth);
  check_layout(wrapped);
  check_same_glyphs(wrapped, unwrapped);
  require(wrapped == vnmaki::layout_text(font, kText, pixel_size, kWrapWidth));

  vnmaki::GlyphAtlas atlas{font};
  try {
    require(atlas.prepare(wrapped));
  } catch (const vnmaki::FontError&) {
    return;  // The font cannot be drawn at this size, or its glyphs do not fit an atlas.
  }
  check_atlas(atlas, wrapped);
  const std::vector<std::uint8_t> pixels = atlas.image().pixels;
  require(!atlas.prepare(unwrapped));
  require(atlas.image().pixels == pixels);
  check_quads(wrapped, atlas);
}

/// Text with tags, two directions and ruby, with the font also as its own fallback: layout is
/// deterministic and finite, and the atlas takes glyphs of more than one size.
void check_styled(const vnmaki::Font& font, float pixel_size) {
  const vnmaki::StyledText text = vnmaki::parse_markup(kMarkup);
  vnmaki::FontSet fonts;
  fonts.base.regular = &font;
  fonts.fallbacks = {&font};
  const vnmaki::TextLayout layout = vnmaki::layout_text(fonts, text, pixel_size, kWrapWidth);
  require(layout == vnmaki::layout_text(fonts, text, pixel_size, kWrapWidth));
  std::size_t glyphs = 0;
  for (const vnmaki::TextLine& line : layout.lines) {
    glyphs += line.glyphs.size();
    require(std::isfinite(line.top) && std::isfinite(line.height) && std::isfinite(line.width));
    for (const vnmaki::LayoutGlyph& glyph : line.glyphs) {
      require(glyph.text_offset < text.text.size());
      require(glyph.font < layout.fonts.size());
      require(std::isfinite(glyph.origin.x) && std::isfinite(glyph.origin.y));
    }
    for (const vnmaki::RubyText& ruby : line.ruby) {
      require(ruby.base_glyph < line.glyphs.size());
    }
  }
  require(glyphs == layout.glyph_count);
  require(vnmaki::revealed_glyphs(layout, 1, 0) == 0);

  vnmaki::GlyphAtlas atlas;
  try {
    require(atlas.prepare(layout));
  } catch (const vnmaki::FontError&) {
    return;
  }
  require(!atlas.prepare(layout));
  for (const vnmaki::Quad& quad :
       vnmaki::text_quads(layout, atlas, vnmaki::PointF{}, layout.glyph_count)) {
    for (const vnmaki::PointF& corner : quad.corners) {
      require(std::isfinite(corner.x) && std::isfinite(corner.y));
    }
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::byte> bytes = std::as_bytes(std::span{data, size});
  try {
    const vnmaki::Font font{std::vector<std::byte>(bytes.begin(), bytes.end())};
    for (const float pixel_size : kPixelSizes) {
      check_font(font, pixel_size);
      check_styled(font, pixel_size);
    }
  } catch (const vnmaki::FontError&) {
    return 0;  // Rejecting bad data is the expected outcome for most inputs.
  }
  return 0;
}
