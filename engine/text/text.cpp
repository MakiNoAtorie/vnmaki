#include "engine/text/text.hpp"

#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <freetype/freetype.h>
#include <hb.h>
#include <linebreak.h>

#include "engine/vfs/vfs.hpp"

namespace vnmaki {

namespace {

/// FreeType and HarfBuzz count in 1/64 pixel.
constexpr float kSubpixels = 64;

struct FreeLibrary {
  void operator()(FT_Library library) const { FT_Done_FreeType(library); }
};
struct FreeFace {
  void operator()(FT_Face face) const { FT_Done_Face(face); }
};
struct FreeShapingFace {
  void operator()(hb_face_t* face) const { hb_face_destroy(face); }
};
struct FreeShapingFont {
  void operator()(hb_font_t* font) const { hb_font_destroy(font); }
};
struct FreeBuffer {
  void operator()(hb_buffer_t* buffer) const { hb_buffer_destroy(buffer); }
};

/// `pixel_size` in 1/64 pixel. Throws std::invalid_argument for a size outside the limits.
int subpixel_size(float pixel_size) {
  if (!(pixel_size > 0 && pixel_size <= kMaxFontPixelSize)) {
    throw std::invalid_argument{"font pixel size must be over 0 and at most " +
                                std::to_string(kMaxFontPixelSize)};
  }
  return std::max(1, static_cast<int>(std::lround(pixel_size * kSubpixels)));
}

/// Shapes text[start, end), a paragraph without its line end, into `buffer`, then appends its
/// lines to `layout`. `breaks` is libunibreak's answer for each byte of `text`.
void append_paragraph(TextLayout& layout, hb_font_t* shaping_font, hb_buffer_t* buffer,
                      std::string_view text, std::span<const char> breaks, std::size_t start,
                      std::size_t end, float wrap_width, float ascent) {
  hb_buffer_clear_contents(buffer);
  hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()),
                     static_cast<unsigned>(start), static_cast<int>(end - start));
  // ponytail: one left-to-right run per paragraph, script guessed from its first letter. Bidi
  // and script/font runs come with ticket 17.
  // A fixed language, so the user's locale cannot change the shaping.
  hb_buffer_set_language(buffer, hb_language_from_string("und", -1));
  hb_buffer_guess_segment_properties(buffer);
  hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
  hb_shape(shaping_font, buffer, nullptr, 0);

  unsigned count = 0;
  const hb_glyph_info_t* first_info = hb_buffer_get_glyph_infos(buffer, &count);
  const hb_glyph_position_t* first_position = hb_buffer_get_glyph_positions(buffer, &count);
  const std::span<const hb_glyph_info_t> infos{first_info, count};
  const std::span<const hb_glyph_position_t> positions{first_position, count};

  const auto advance = [&](std::size_t i) {
    return static_cast<float>(positions[i].x_advance) / kSubpixels;
  };
  // A glyph can start a line only if it starts a cluster (a ligature or a letter with its marks
  // stays whole).
  const auto starts_cluster = [&](std::size_t i) {
    return i == 0 || infos[i].cluster != infos[i - 1].cluster;
  };
  const auto may_break_before = [&](std::size_t i) {
    const std::size_t offset = infos[i].cluster;
    return starts_cluster(i) && offset > 0 && breaks[offset - 1] == LINEBREAK_ALLOWBREAK;
  };
  const auto is_space = [&](std::size_t i) { return text[infos[i].cluster] == ' '; };
  const auto add_line = [&](std::size_t first, std::size_t last) {
    TextLine line;
    line.baseline = (static_cast<float>(layout.lines.size()) * layout.line_height) + ascent;
    line.glyphs.reserve(last - first);
    float pen = 0;
    for (std::size_t i = first; i < last; ++i) {
      line.glyphs.push_back(LayoutGlyph{
          .glyph = infos[i].codepoint,
          .text_offset = infos[i].cluster,
          .origin = {
              .x = pen + (static_cast<float>(positions[i].x_offset) / kSubpixels),
              .y = line.baseline - (static_cast<float>(positions[i].y_offset) / kSubpixels)}});
      pen += advance(i);
      if (!is_space(i)) {
        line.width = pen;
      }
    }
    layout.glyph_count += line.glyphs.size();
    layout.lines.push_back(std::move(line));
  };

  std::size_t line_start = 0;
  std::optional<std::size_t> break_at;  // The last place the current line may break.
  float width = 0;
  for (std::size_t i = 0; i < infos.size(); ++i) {
    if (i > line_start && may_break_before(i)) {
      break_at = i;
    }
    while (i > line_start && !is_space(i) && width + advance(i) > wrap_width) {
      if (!break_at) {
        // No break is allowed in this line: split the word here, if a glyph may start a line.
        if (!starts_cluster(i)) {
          break;
        }
        break_at = i;
      }
      add_line(line_start, *break_at);
      line_start = *break_at;
      break_at.reset();
      width = 0;
      for (std::size_t moved = line_start; moved < i; ++moved) {
        width += advance(moved);
      }
    }
    width += advance(i);
  }
  add_line(line_start, infos.size());
}

/// Renders `glyph` at the size set on `face`. Leaves the outputs empty for a glyph without
/// pixels, or one that cannot be rendered or extends over `max_extent` (1/64 pixel).
void rasterize(FT_Face face, std::uint32_t glyph, FT_Pos max_extent, AtlasGlyph& placement,
               std::vector<std::uint8_t>& coverage) {
  // No hinting: glyphs keep the advances HarfBuzz laid them out with. No embedded bitmaps: they
  // exist only for some sizes.
  constexpr FT_Int32 kLoadFlags = FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP;
  if (FT_Load_Glyph(face, glyph, kLoadFlags) != 0) {
    return;
  }
  const FT_GlyphSlot slot = face->glyph;
  if (slot->format != FT_GLYPH_FORMAT_OUTLINE || slot->metrics.width > max_extent ||
      slot->metrics.height > max_extent) {
    return;
  }
  if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) {
    return;
  }
  const FT_Bitmap& bitmap = slot->bitmap;
  const std::size_t width = bitmap.width;
  const std::size_t height = bitmap.rows;
  if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY || bitmap.buffer == nullptr || width == 0 ||
      height == 0 || bitmap.pitch < 0 || static_cast<std::size_t>(bitmap.pitch) < width) {
    return;
  }
  const auto pitch = static_cast<std::size_t>(bitmap.pitch);
  const std::span<const unsigned char> rows{bitmap.buffer, height * pitch};
  coverage.reserve(width * height);
  for (std::size_t row = 0; row < height; ++row) {
    const std::span<const unsigned char> pixels = rows.subspan(row * pitch, width);
    coverage.insert(coverage.end(), pixels.begin(), pixels.end());
  }
  placement.width = static_cast<int>(width);
  placement.height = static_cast<int>(height);
  placement.left = slot->bitmap_left;
  placement.top = slot->bitmap_top;
}

}  // namespace

struct Font::Impl {
  explicit Impl(std::vector<std::byte> bytes) : data(std::move(bytes)) {
    if (data.empty() || data.size() > static_cast<std::size_t>(INT_MAX)) {
      throw FontError{"not a font"};
    }
    FT_Library new_library = nullptr;
    if (FT_Init_FreeType(&new_library) != 0) {
      throw FontError{"cannot start FreeType"};
    }
    library.reset(new_library);
    // FreeType reads unsigned chars; std::byte storage may be read through them.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* font_bytes = reinterpret_cast<const FT_Byte*>(data.data());
    FT_Face new_face = nullptr;
    if (FT_New_Memory_Face(library.get(), font_bytes, static_cast<FT_Long>(data.size()), 0,
                           &new_face) != 0) {
      throw FontError{"not a font"};
    }
    face.reset(new_face);
    if ((face->face_flags & FT_FACE_FLAG_SCALABLE) == 0) {
      throw FontError{"not a scalable font"};
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* blob_bytes = reinterpret_cast<const char*>(data.data());
    hb_blob_t* blob = hb_blob_create(blob_bytes, static_cast<unsigned>(data.size()),
                                     HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    shaping_face.reset(hb_face_create(blob, 0));
    hb_blob_destroy(blob);
  }

  /// The font file. FreeType and HarfBuzz both read from it for as long as the faces live.
  std::vector<std::byte> data;
  std::unique_ptr<std::remove_pointer_t<FT_Library>, FreeLibrary> library;
  std::unique_ptr<std::remove_pointer_t<FT_Face>, FreeFace> face;
  std::unique_ptr<hb_face_t, FreeShapingFace> shaping_face;
};

Font::Font(std::vector<std::byte> data) : impl_(std::make_unique<Impl>(std::move(data))) {}
Font::Font(Font&&) noexcept = default;
Font& Font::operator=(Font&&) noexcept = default;
Font::~Font() = default;

Font load_font(const Vfs& vfs, std::string_view path) {
  std::optional<std::vector<std::byte>> data = vfs.read(path);
  if (!data) {
    throw FontError{"cannot read font '" + std::string{path} + "'"};
  }
  try {
    return Font{std::move(*data)};
  } catch (const FontError& error) {
    throw FontError{"cannot load font '" + std::string{path} + "': " + error.what()};
  }
}

TextLayout layout_text(const Font& font, std::string_view text, float pixel_size,
                       float wrap_width) {
  const int scale = subpixel_size(pixel_size);
  if (text.size() > static_cast<std::size_t>(INT_MAX)) {
    throw std::invalid_argument{"text is too long"};
  }
  const std::unique_ptr<hb_font_t, FreeShapingFont> shaping_font{
      hb_font_create(font.impl_->shaping_face.get())};
  hb_font_set_scale(shaping_font.get(), scale, scale);
  hb_font_extents_t extents{};
  hb_font_get_h_extents(shaping_font.get(), &extents);

  TextLayout layout;
  layout.pixel_size = pixel_size;
  layout.line_height =
      static_cast<float>(extents.ascender - extents.descender + extents.line_gap) / kSubpixels;
  if (text.empty()) {
    return layout;
  }
  const float ascent = static_cast<float>(extents.ascender) / kSubpixels;

  std::vector<char> breaks(text.size());
  // libunibreak reads unsigned chars; char storage may be read through them.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* units = reinterpret_cast<const utf8_t*>(text.data());
  set_linebreaks_utf8(units, text.size(), nullptr, breaks.data());

  const std::unique_ptr<hb_buffer_t, FreeBuffer> buffer{hb_buffer_create()};
  std::size_t start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (breaks[i] != LINEBREAK_MUSTBREAK && i + 1 < text.size()) {
      continue;
    }
    // A paragraph ends after text[i]. Its line end is not shaped.
    std::size_t end = i + 1;
    while (end > start && (text[end - 1] == '\n' || text[end - 1] == '\r')) {
      --end;
    }
    append_paragraph(layout, shaping_font.get(), buffer.get(), text, breaks, start, end, wrap_width,
                     ascent);
    start = i + 1;
  }
  return layout;
}

std::size_t revealed_glyphs(std::size_t glyph_count, double characters_per_second,
                            double elapsed_seconds) {
  if (!(characters_per_second > 0)) {
    return glyph_count;
  }
  if (!(elapsed_seconds > 0)) {
    return 0;
  }
  const double shown = std::floor(characters_per_second * elapsed_seconds);
  return shown >= static_cast<double>(glyph_count) ? glyph_count : static_cast<std::size_t>(shown);
}

GlyphAtlas::GlyphAtlas(const Font& font) : font_(&font) { (void)pack(); }

bool GlyphAtlas::prepare(const TextLayout& layout, float scale) {
  const float new_size = layout.pixel_size * scale;
  const int size = subpixel_size(new_size);
  bool changed = false;
  if (new_size != pixel_size_) {
    glyphs_.clear();
    pixel_size_ = new_size;
    changed = true;
  }
  const FT_Face face = font_->impl_->face.get();
  if (FT_Set_Char_Size(face, 0, size, 0, 0) != 0) {
    glyphs_.clear();
    (void)pack();
    throw FontError{"the font cannot be drawn at " + std::to_string(new_size) + " pixels"};
  }
  const FT_Pos max_extent = FT_Pos{size} * kMaxGlyphEms;
  for (const TextLine& line : layout.lines) {
    for (const LayoutGlyph& glyph : line.glyphs) {
      const auto [entry, added] = glyphs_.try_emplace(glyph.glyph);
      if (added) {
        rasterize(face, glyph.glyph, max_extent, entry->second.placement, entry->second.coverage);
        changed = true;
      }
    }
  }
  if (changed && !pack()) {
    glyphs_.clear();
    (void)pack();
    throw FontError{"the glyph atlas would be larger than " + std::to_string(kMaxImageDimension) +
                    " pixels"};
  }
  return changed;
}

const AtlasGlyph* GlyphAtlas::find(std::uint32_t glyph) const {
  const auto found = glyphs_.find(glyph);
  return found == glyphs_.end() ? nullptr : &found->second.placement;
}

bool GlyphAtlas::pack() {
  // Transparent pixels around every bitmap, so a neighbour never bleeds into a glyph's edge.
  constexpr int kBorder = 1;
  constexpr std::size_t kChannels = 4;
  std::int64_t area = 0;
  int widest = 0;
  for (const auto& [glyph, bitmap] : glyphs_) {
    const AtlasGlyph& placed = bitmap.placement;
    area += std::int64_t{placed.width + kBorder} * (placed.height + kBorder);
    widest = std::max(widest, placed.width);
  }
  // Rows of glyphs in a power-of-two width that makes the image about square.
  const double side = std::max(std::ceil(std::sqrt(static_cast<double>(area))),
                               static_cast<double>(widest + (2 * kBorder)));
  if (side > kMaxImageDimension) {
    return false;
  }
  const int width = static_cast<int>(std::bit_ceil(static_cast<unsigned>(side)));
  int x = kBorder;
  int y = kBorder;
  int row_height = 0;
  for (auto& [glyph, bitmap] : glyphs_) {
    AtlasGlyph& placed = bitmap.placement;
    if (placed.width == 0) {
      continue;
    }
    if (x + placed.width + kBorder > width) {
      x = kBorder;
      y += row_height + kBorder;
      row_height = 0;
    }
    if (y + placed.height + kBorder > kMaxImageDimension) {
      return false;
    }
    placed.x = x;
    placed.y = y;
    x += placed.width + kBorder;
    row_height = std::max(row_height, placed.height);
  }
  const int height = y + row_height + kBorder;

  Image packed;
  packed.size = Size{.width = width, .height = height};
  packed.pixels.assign(
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * kChannels, 0);
  for (const auto& [glyph, bitmap] : glyphs_) {
    const AtlasGlyph& placed = bitmap.placement;
    const auto glyph_width = static_cast<std::size_t>(placed.width);
    for (std::size_t offset = 0; offset < bitmap.coverage.size(); ++offset) {
      const std::size_t pixel_x = static_cast<std::size_t>(placed.x) + (offset % glyph_width);
      const std::size_t pixel_y = static_cast<std::size_t>(placed.y) + (offset / glyph_width);
      const std::size_t pixel = (pixel_y * static_cast<std::size_t>(width)) + pixel_x;
      // White at this coverage, premultiplied: all four channels are the coverage.
      std::fill_n(packed.pixels.begin() + static_cast<std::ptrdiff_t>(pixel * kChannels), kChannels,
                  bitmap.coverage[offset]);
    }
  }
  image_ = std::move(packed);
  return true;
}

std::vector<Quad> text_quads(const TextLayout& layout, const GlyphAtlas& atlas, PointF origin,
                             std::size_t visible_glyphs, Color tint) {
  std::vector<Quad> quads;
  if (!(layout.pixel_size > 0)) {
    return quads;
  }
  quads.reserve(std::min(visible_glyphs, layout.glyph_count));
  const float scale = atlas.pixel_size() / layout.pixel_size;
  const auto atlas_width = static_cast<float>(atlas.image().size.width);
  const auto atlas_height = static_cast<float>(atlas.image().size.height);
  std::size_t remaining = visible_glyphs;
  for (const TextLine& line : layout.lines) {
    for (const LayoutGlyph& glyph : line.glyphs) {
      if (remaining == 0) {
        return quads;
      }
      --remaining;
      const AtlasGlyph* placed = atlas.find(glyph.glyph);
      if (placed == nullptr || placed->width == 0) {
        continue;
      }
      // Whole pixels, so the bitmap's pixels land on the target's pixels.
      const float x = std::round(origin.x + (glyph.origin.x * scale));
      const float y = std::round(origin.y + (glyph.origin.y * scale));
      quads.push_back(
          quad_from_rect(RectF{.x = x + static_cast<float>(placed->left),
                               .y = y - static_cast<float>(placed->top),
                               .width = static_cast<float>(placed->width),
                               .height = static_cast<float>(placed->height)},
                         RectF{.x = static_cast<float>(placed->x) / atlas_width,
                               .y = static_cast<float>(placed->y) / atlas_height,
                               .width = static_cast<float>(placed->width) / atlas_width,
                               .height = static_cast<float>(placed->height) / atlas_height},
                         tint));
    }
  }
  return quads;
}

}  // namespace vnmaki
