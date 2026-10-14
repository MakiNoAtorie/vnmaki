#pragma once

// Text (docs/SPEC.md section 13): fonts, paragraph layout, the glyph atlas and typewriter reveal.
// HarfBuzz shapes, SheenBidi gives the text directions, libunibreak finds the line breaks and
// FreeType rasterizes; none of them shows in this header. The inline tags are in markup.hpp.

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "engine/platform/geometry.hpp"
#include "engine/render/image.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/shader.hpp"
#include "engine/text/markup.hpp"

namespace vnmaki {

class Vfs;

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
  friend struct FontFaces;
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
  /// The glyph's font: an index in TextLayout::fonts.
  std::uint16_t font = 0;
  /// The glyph is part of right-to-left text: its leading edge is its right edge.
  bool rtl = false;
  /// The glyph's size in pixels per em, and how far it moves the pen.
  float pixel_size = 0;
  float advance = 0;
  /// The colour from a `color` tag. Not set: the tint given to text_quads.
  std::optional<Color> color;
  /// Typewriter reveal: the seconds to wait before this glyph shows (`pause` tags), and its
  /// speed in characters per second (`speed` tag; 0: the speed of the reveal).
  double reveal_pause = 0;
  double reveal_speed = 0;

  bool operator==(const LayoutGlyph&) const = default;
};

/// Furigana: small glyphs above the base glyphs of a `ruby` tag. They show together with the
/// first base glyph, and the reveal does not count them.
struct RubyText {
  /// The index in TextLine::glyphs of the first base glyph.
  std::size_t base_glyph = 0;
  std::vector<LayoutGlyph> glyphs;

  bool operator==(const RubyText&) const = default;
};

/// Where the text of a `link` tag is, for pointer hit tests: one region for each run of the
/// link on each line.
struct LinkRegion {
  std::string id;
  /// From the layout's top-left corner; the full height of the line.
  RectF rect{};

  bool operator==(const LinkRegion&) const = default;
};

struct TextLine {
  /// In logical (reading and reveal) order. In right-to-left text the first glyph is the
  /// rightmost one.
  std::vector<LayoutGlyph> glyphs;
  /// The baseline's distance below the layout's top.
  float baseline = 0;
  /// The line's advance width, without spaces at its end.
  float width = 0;
  /// The line's top edge below the layout's top, and its height. A line with larger glyphs or
  /// with ruby is higher than TextLayout::line_height.
  float top = 0;
  float height = 0;
  std::vector<RubyText> ruby;

  bool operator==(const TextLine&) const = default;
};

/// Text laid out in lines. All lengths are in the pixels of `pixel_size`.
struct TextLayout {
  float pixel_size = 0;
  /// The distance from one baseline to the next, for lines of the base font at `pixel_size`.
  float line_height = 0;
  std::vector<TextLine> lines;
  /// The glyphs of all lines together: what a typewriter reveal counts up to.
  std::size_t glyph_count = 0;
  /// The fonts the glyphs use (LayoutGlyph::font). They must outlive the layout's use.
  std::vector<const Font*> fonts;
  std::vector<LinkRegion> links;

  bool operator==(const TextLayout&) const = default;
};

/// Shapes UTF-8 `text` at `pixel_size` pixels per em and breaks it into lines no wider than
/// `wrap_width` pixels, at the places the Unicode line breaking rules allow (so Japanese wraps
/// without spaces, and never before a closing mark such as "。"). A word wider than a line is
/// split between characters. Line ends in the text always break. Spaces at a line's end hang over
/// the width. The text has one font and no tags; see the FontSet overload for the rest.
/// Throws std::invalid_argument unless 0 < pixel_size <= kMaxFontPixelSize.
[[nodiscard]] TextLayout layout_text(const Font& font, std::string_view text, float pixel_size,
                                     float wrap_width);

/// The fonts of one family. A style without its own font uses the nearest one that is set
/// (bold italic text uses `bold`, then `italic`, then `regular`).
struct FontFamily {
  const Font* regular = nullptr;
  const Font* bold = nullptr;
  const Font* italic = nullptr;
  const Font* bold_italic = nullptr;
};

/// The fonts a styled text is laid out with. They must outlive the layouts made with them.
struct FontSet {
  /// The family of text without a `font` tag. `base.regular` must be set.
  FontFamily base;
  /// The families a `font` tag can name. A name that is not here uses `base`.
  std::map<std::string, FontFamily, std::less<>> families;
  /// The fallback chain: a character that the style's font does not have uses the first of
  /// these fonts that has it. Build it from ProjectConfig::font_fallbacks for the language.
  std::vector<const Font*> fallbacks;
};

/// As layout_text above, for text with styles (see parse_markup) and more than one font.
/// - `size`, `font`, `b` and `i` select the size and font of each part. A line is as high as
///   its largest glyphs need.
/// - Right-to-left text (Hebrew, Arabic) and mixed text are put in visual order with the
///   Unicode bidirectional algorithm. TextLine::glyphs stays in logical order.
/// - A small kana or a long vowel mark does not start a line (kinsoku).
/// - Ruby text is half the size of its base, centred above it. A base that is narrower than its
///   ruby gets space on both sides. A line does not break in a ruby base.
/// Throws std::invalid_argument for a size outside (0, kMaxFontPixelSize] or without
/// `fonts.base.regular`.
[[nodiscard]] TextLayout layout_text(const FontSet& fonts, const StyledText& text, float pixel_size,
                                     float wrap_width);

/// How many glyphs a typewriter reveal shows `elapsed_seconds` after it started, at
/// `characters_per_second`: none before the start, all `glyph_count` once they have had time.
/// A speed that is not positive shows everything at once.
[[nodiscard]] std::size_t revealed_glyphs(std::size_t glyph_count, double characters_per_second,
                                          double elapsed_seconds);

/// As revealed_glyphs above, with the `speed` and `pause` tags of the layout's text: a glyph
/// with its own speed takes 1 / speed seconds, and a pause comes before its glyph. A
/// `characters_per_second` that is not positive still shows everything at once.
[[nodiscard]] std::size_t revealed_glyphs(const TextLayout& layout, double characters_per_second,
                                          double elapsed_seconds);

/// Where a text caret goes for the byte `text_offset` of the layout's text: the leading edge of
/// that character's glyph (its right edge in right-to-left text), on the baseline. An offset at
/// or after the end gives the trailing edge of the last glyph.
[[nodiscard]] PointF caret_position(const TextLayout& layout, std::size_t text_offset);

/// The id of the link at `point` (from the layout's top-left corner), or an empty string.
[[nodiscard]] std::string_view link_at(const TextLayout& layout, PointF point);

/// What a GlyphAtlas holds for each glyph.
enum class GlyphRaster : std::uint8_t {
  /// Coverage: normal text, drawn with the sprite shader.
  kCoverage,
  /// A signed distance field: effect text (outline, shadow), drawn with sdf_text_shader().
  kDistance,
};

/// How far a distance-field bitmap reaches around its glyph, in atlas pixels. An outline must be
/// narrower than this.
inline constexpr int kSdfSpread = 8;

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
  /// An atlas for layouts that name their fonts (every layout_text result does). Those fonts
  /// must outlive the atlas.
  explicit GlyphAtlas(GlyphRaster raster = GlyphRaster::kCoverage);
  /// `font` must outlive the atlas, as must the fonts of every layout given to prepare().
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
  /// White glyphs with coverage as alpha (premultiplied), each with a transparent border. For
  /// GlyphRaster::kDistance, all four channels hold the distance: 128 on the glyph's edge, 16
  /// more for each pixel inside it and 16 less for each pixel outside (kSdfSpread pixels at most).
  [[nodiscard]] const Image& image() const { return image_; }
  /// The entry for a glyph index of the constructor's font at pixel_size(), or null if prepare()
  /// has not seen it.
  [[nodiscard]] const AtlasGlyph* find(std::uint32_t glyph) const;
  /// The entry for a glyph of `layout`, or null if the last prepare() did not have it.
  [[nodiscard]] const AtlasGlyph* find(const TextLayout& layout, const LayoutGlyph& glyph) const;

 private:
  struct Key {
    /// An index in fonts_.
    std::size_t font = 0;
    /// In 1/64 pixel per em.
    int size = 0;
    std::uint32_t glyph = 0;

    auto operator<=>(const Key&) const = default;
  };
  struct Bitmap {
    AtlasGlyph placement{};
    /// placement.width * placement.height coverage bytes, rows from the top.
    std::vector<std::uint8_t> coverage;
  };

  /// The key of a glyph of `layout`, or nothing for a font that prepare() has not seen or a
  /// size outside the limits.
  [[nodiscard]] std::optional<Key> key_for(const TextLayout& layout,
                                           const LayoutGlyph& glyph) const;
  /// Places the bitmaps and builds image_. Returns false, leaving image_ as it was, if they do
  /// not fit kMaxImageDimension.
  [[nodiscard]] bool pack();

  GlyphRaster raster_ = GlyphRaster::kCoverage;
  /// fonts_[0] is the constructor's font, if it took one.
  std::vector<const Font*> fonts_;
  float pixel_size_ = 0;
  /// Screen pixels per layout pixel at the last prepare().
  float scale_ = 1;
  std::map<Key, Bitmap> glyphs_;
  Image image_;
};

/// The quads that draw the first `visible_glyphs` glyphs of `layout` (see revealed_glyphs) with
/// the top-left corner of the layout at `origin`. `atlas` must have been prepared with `layout`;
/// `origin` and the quads are in the atlas's pixels (target pixels), and every glyph lands on
/// whole pixels. Draw them with the texture made from atlas.image(). A glyph with a `color` tag
/// has that colour in place of `tint`. Ruby glyphs come with their first base glyph.
[[nodiscard]] std::vector<Quad> text_quads(const TextLayout& layout, const GlyphAtlas& atlas,
                                           PointF origin, std::size_t visible_glyphs,
                                           Color tint = kWhite);

/// The fragment shader that draws quads of a GlyphRaster::kDistance atlas: the glyph in the
/// quad's tint, with an outline around it. Make it with IRenderer::create_shader and pass an
/// SdfTextUniforms as DrawState::uniforms. For a shadow, draw the same quads first, moved and
/// tinted, with the outline in the shadow's colour.
[[nodiscard]] ShaderBytecode sdf_text_shader();

/// The constant buffer of sdf_text_shader() (two 16-byte rows).
struct SdfTextUniforms {
  /// Straight alpha.
  std::array<float, 4> outline_color{};
  /// In atlas pixels; 0 for no outline.
  float outline_width = 0;
  /// The pixels of distance that one step of an 8-bit atlas value stands for.
  float pixels_per_level = 0;
  std::array<float, 2> unused_padding{};
};

/// Uniforms for an outline `outline_width` atlas pixels wide (at most kSdfSpread - 1).
[[nodiscard]] SdfTextUniforms sdf_text_uniforms(Color outline_color, float outline_width);

}  // namespace vnmaki
