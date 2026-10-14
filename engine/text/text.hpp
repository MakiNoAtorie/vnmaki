#pragma once

// Text (docs/SPEC.md section 13): fonts, paragraph layout, the glyph atlas and typewriter reveal.
// HarfBuzz shapes, libunibreak finds the line breaks and FreeType rasterizes; none of them shows
// in this header.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "engine/platform/geometry.hpp"
#include "engine/render/image.hpp"
#include "engine/render/renderer.hpp"

namespace vnmaki {

class Vfs;

/// The largest font size, in pixels per em, that layout and the glyph atlas accept.
inline constexpr float kMaxFontPixelSize = 1024;
/// A glyph wider or taller than this many ems is not drawn, so a hostile font cannot make the
/// atlas allocate huge bitmaps.
inline constexpr int kMaxGlyphEms = 16;
/// A wrap width for text that breaks only at line ends in the text itself.
inline constexpr float kNoWrap = std::numeric_limits<float>::infinity();

/// Thrown when font data cannot be used, or the glyph atlas would pass kMaxImageDimension. The
/// message names the reason (and the path, for load_font).
class FontError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct TextLayout;

/// A scalable font (TrueType or OpenType) held in memory. Use a Font, and everything made from
/// it, from one thread at a time.
class Font {
 public:
  /// Takes a whole font file. Throws FontError if it is not a scalable font.
  explicit Font(std::vector<std::byte> data);
  Font(const Font&) = delete;
  Font& operator=(const Font&) = delete;
  Font(Font&&) noexcept;
  Font& operator=(Font&&) noexcept;
  ~Font();

 private:
  struct Impl;
  friend TextLayout layout_text(const Font& font, std::string_view text, float pixel_size,
                                float wrap_width);
  friend class GlyphAtlas;
  std::unique_ptr<Impl> impl_;
};

/// Reads the font at virtual path `path` through `vfs`. Throws FontError if the file is missing
/// or is not a scalable font.
[[nodiscard]] Font load_font(const Vfs& vfs, std::string_view path);

/// One glyph of laid-out text.
struct LayoutGlyph {
  /// The glyph's index in the font; 0 is the font's "missing character" glyph.
  std::uint32_t glyph = 0;
  /// The byte offset in the text of the first character this glyph draws.
  std::uint32_t text_offset = 0;
  /// Where the glyph's origin (its left end on the baseline) lies, from the layout's top-left
  /// corner, y down.
  PointF origin{};

  bool operator==(const LayoutGlyph&) const = default;
};

struct TextLine {
  /// In reading order.
  std::vector<LayoutGlyph> glyphs;
  /// The baseline's distance below the layout's top.
  float baseline = 0;
  /// The line's advance width, without spaces at its end.
  float width = 0;

  bool operator==(const TextLine&) const = default;
};

/// Text laid out in lines. All lengths are in the pixels of `pixel_size`.
struct TextLayout {
  float pixel_size = 0;
  /// The distance from one baseline to the next.
  float line_height = 0;
  std::vector<TextLine> lines;
  /// The glyphs of all lines together: what a typewriter reveal counts up to.
  std::size_t glyph_count = 0;

  bool operator==(const TextLayout&) const = default;
};

/// Shapes UTF-8 `text` at `pixel_size` pixels per em and breaks it into lines no wider than
/// `wrap_width` pixels, at the places the Unicode line breaking rules allow (so Japanese wraps
/// without spaces, and never before a closing mark such as "。"). A word wider than a line is
/// split between characters. Line ends in the text always break. Spaces at a line's end hang over
/// the width. Text runs left to right in one font.
/// Throws std::invalid_argument unless 0 < pixel_size <= kMaxFontPixelSize.
[[nodiscard]] TextLayout layout_text(const Font& font, std::string_view text, float pixel_size,
                                     float wrap_width);

/// How many glyphs a typewriter reveal shows `elapsed_seconds` after it started, at
/// `characters_per_second`: none before the start, all `glyph_count` once they have had time.
/// A speed that is not positive shows everything at once.
[[nodiscard]] std::size_t revealed_glyphs(std::size_t glyph_count, double characters_per_second,
                                          double elapsed_seconds);

/// Where a glyph's bitmap is in the atlas image, and where it goes when drawn.
struct AtlasGlyph {
  /// The bitmap's rectangle in the atlas image, in pixels. Empty for a glyph without pixels.
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  /// From the glyph's origin to the bitmap's left edge (pixels, rightwards) and to its top edge
  /// (pixels, upwards).
  int left = 0;
  int top = 0;

  bool operator==(const AtlasGlyph&) const = default;
};

/// The bitmaps of the glyphs in use, rasterized at the size they have on screen and packed into
/// one image for IRenderer::create_texture.
class GlyphAtlas {
 public:
  /// `font` must outlive the atlas.
  explicit GlyphAtlas(const Font& font);

  /// Makes the atlas hold every glyph of `layout`, rasterized at layout.pixel_size * scale pixels
  /// per em. `scale` is screen pixels per layout pixel: pass the viewport scale, so text stays
  /// sharp when the window is resized or the display's DPI changes. A new size discards the
  /// glyphs of the old one. Returns whether image() changed and must be uploaded again.
  /// Throws std::invalid_argument for a size outside (0, kMaxFontPixelSize]. Throws FontError,
  /// and drops every glyph, if the font cannot be drawn at that size or the image would pass
  /// kMaxImageDimension.
  bool prepare(const TextLayout& layout, float scale = 1);

  /// The size the glyphs are rasterized at, in pixels per em; 0 before the first prepare().
  [[nodiscard]] float pixel_size() const { return pixel_size_; }
  /// White glyphs with coverage as alpha (premultiplied), each with a transparent border.
  [[nodiscard]] const Image& image() const { return image_; }
  /// The entry for a glyph index, or null if prepare() has not seen it.
  [[nodiscard]] const AtlasGlyph* find(std::uint32_t glyph) const;

 private:
  struct Bitmap {
    AtlasGlyph placement{};
    /// placement.width * placement.height coverage bytes, rows from the top.
    std::vector<std::uint8_t> coverage;
  };

  /// Places the bitmaps and builds image_. Returns false, leaving image_ as it was, if they do
  /// not fit kMaxImageDimension.
  [[nodiscard]] bool pack();

  const Font* font_ = nullptr;
  float pixel_size_ = 0;
  std::map<std::uint32_t, Bitmap> glyphs_;
  Image image_;
};

/// The quads that draw the first `visible_glyphs` glyphs of `layout` (see revealed_glyphs) with
/// the top-left corner of the layout at `origin`. `atlas` must have been prepared with `layout`;
/// `origin` and the quads are in the atlas's pixels (target pixels), and every glyph lands on
/// whole pixels. Draw them with the texture made from atlas.image().
[[nodiscard]] std::vector<Quad> text_quads(const TextLayout& layout, const GlyphAtlas& atlas,
                                           PointF origin, std::size_t visible_glyphs,
                                           Color tint = kWhite);

}  // namespace vnmaki
