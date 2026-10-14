#include "engine/text/text.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <SheenBidi/SheenBidi.h>
#include <freetype/freetype.h>
#include <hb.h>
#include <linebreak.h>

#include "engine/render/builtin_shaders.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki {

namespace {

/// FreeType and HarfBuzz count in 1/64 pixel.
constexpr int kSubpixelsPerPixel = 64;
constexpr auto kSubpixels = static_cast<float>(kSubpixelsPerPixel);

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
  std::vector<std::byte> data{};
  std::unique_ptr<std::remove_pointer_t<FT_Library>, FreeLibrary> library;
  std::unique_ptr<std::remove_pointer_t<FT_Face>, FreeFace> face;
  std::unique_ptr<hb_face_t, FreeShapingFace> shaping_face;
};

/// The library objects of a Font, for the code in this file.
struct FontFaces {
  static hb_face_t* shaping(const Font& font) { return font.impl_->shaping_face.get(); }
  static FT_Face face(const Font& font) { return font.impl_->face.get(); }
};

namespace {

/// Ruby text is this much of its base text's size.
constexpr float kRubyScale = 0.5F;
constexpr float kHalf = 0.5F;
/// For a byte of text that no StyledRun covers.
constexpr std::size_t kNoRun = std::numeric_limits<std::size_t>::max();

/// Characters that do not start a line in Japanese text (kinsoku), in addition to the ones the
/// Unicode line breaking rules already hold back: small kana and the long vowel mark. Unicode
/// lets a line break before them by default (line break class CJ).
constexpr std::u32string_view kNoLineStart =
    U"ぁぃぅぇぉっゃゅょゎゕゖァィゥェォッャュョヮヵヶーㇰㇱㇲㇳㇴㇵㇶㇷㇸㇹㇺㇻㇼㇽㇾㇿｧｨｩｪｫｬｭｮｯｰ";

/// `pixel_size` in 1/64 pixel. Throws std::invalid_argument for a size outside the limits.
int subpixel_size(float pixel_size) {
  if (!(pixel_size > 0 && pixel_size <= kMaxFontPixelSize)) {
    throw std::invalid_argument{"font pixel size must be over 0 and at most " +
                                std::to_string(kMaxFontPixelSize)};
  }
  return std::max(1, static_cast<int>(std::lround(pixel_size * kSubpixels)));
}

struct Decoded {
  std::uint32_t code = 0;
  std::size_t length = 1;
};

/// The code point that starts at byte `at` of `text`. A byte that starts no UTF-8 sequence is
/// U+FFFD, one byte long.
Decoded decode_utf8(std::string_view text, std::size_t at) {
  struct Form {
    std::uint32_t mask = 0;
    std::uint32_t lead = 0;
    std::size_t length = 0;
  };
  constexpr std::array<Form, 4> kForms{{{.mask = 0x80, .lead = 0x00, .length = 1},
                                        {.mask = 0xE0, .lead = 0xC0, .length = 2},
                                        {.mask = 0xF0, .lead = 0xE0, .length = 3},
                                        {.mask = 0xF8, .lead = 0xF0, .length = 4}}};
  constexpr std::uint32_t kByteMask = 0xFF;
  constexpr std::uint32_t kTailMask = 0xC0;
  constexpr std::uint32_t kTail = 0x80;
  constexpr std::uint32_t kTailBits = 6;
  constexpr std::uint32_t kReplacement = 0xFFFD;
  const auto byte = [&](std::size_t i) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(text[i]));
  };
  const std::uint32_t first = byte(at);
  for (const Form& form : kForms) {
    if ((first & form.mask) != form.lead || form.length > text.size() - at) {
      continue;
    }
    std::uint32_t code = first & ~form.mask & kByteMask;
    for (std::size_t i = 1; i < form.length; ++i) {
      const std::uint32_t tail = byte(at + i);
      if ((tail & kTailMask) != kTail) {
        return {.code = kReplacement, .length = 1};
      }
      code = (code << kTailBits) | (tail & ~kTailMask & kByteMask);
    }
    return {.code = code, .length = form.length};
  }
  return {.code = kReplacement, .length = 1};
}

/// libunibreak's answer for each byte of `text`, with the kinsoku rule added.
std::vector<char> line_breaks(std::string_view text) {
  std::vector<char> breaks(text.size());
  // libunibreak reads unsigned chars; char storage may be read through them.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* units = reinterpret_cast<const utf8_t*>(text.data());
  set_linebreaks_utf8(units, text.size(), nullptr, breaks.data());
  for (std::size_t i = 0; i < text.size();) {
    const Decoded character = decode_utf8(text, i);
    // The answer for a break before a character is at the byte before it.
    if (i > 0 && breaks[i - 1] == LINEBREAK_ALLOWBREAK &&
        kNoLineStart.find(static_cast<char32_t>(character.code)) != std::u32string_view::npos) {
      breaks[i - 1] = LINEBREAK_NOBREAK;
    }
    i += character.length;
  }
  return breaks;
}

/// The direction of each byte of a text, from the Unicode bidirectional algorithm.
struct BidiLevels {
  /// The embedding level: even is left-to-right, odd is right-to-left.
  std::vector<std::uint8_t> level{};
  /// The base level of the byte's paragraph.
  std::vector<std::uint8_t> paragraph{};
};

BidiLevels bidi_levels(std::string_view text) {
  BidiLevels levels{.level = std::vector<std::uint8_t>(text.size(), 0),
                    .paragraph = std::vector<std::uint8_t>(text.size(), 0)};
  const SBCodepointSequence sequence{.stringEncoding = SBStringEncodingUTF8,
                                     .stringBuffer = text.data(),
                                     .stringLength = text.size()};
  const SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence);
  if (algorithm == nullptr) {
    return levels;
  }
  std::size_t offset = 0;
  while (offset < text.size()) {
    const SBParagraphRef paragraph =
        SBAlgorithmCreateParagraph(algorithm, offset, text.size() - offset, SBLevelDefaultLTR);
    if (paragraph == nullptr) {
      break;
    }
    const std::size_t length = std::min<std::size_t>(
        std::max<std::size_t>(SBParagraphGetLength(paragraph), 1), text.size() - offset);
    const std::span<const SBLevel> found{SBParagraphGetLevelsPtr(paragraph), length};
    std::copy(found.begin(), found.end(),
              levels.level.begin() + static_cast<std::ptrdiff_t>(offset));
    std::fill_n(levels.paragraph.begin() + static_cast<std::ptrdiff_t>(offset), length,
                SBParagraphGetBaseLevel(paragraph));
    SBParagraphRelease(paragraph);
    offset += length;
  }
  SBAlgorithmRelease(algorithm);
  return levels;
}

/// One shaped glyph. A paragraph's glyphs are kept in logical order.
struct Shaped {
  std::uint32_t glyph = 0;
  /// The byte offset in the text of the glyph's first character.
  std::uint32_t cluster = 0;
  float advance = 0;
  float x_offset = 0;
  float y_offset = 0;
  float pixel_size = 0;
  /// The font's ascent and line height at pixel_size.
  float ascent = 0;
  float line_height = 0;
  std::uint16_t font = 0;
  std::uint8_t level = 0;
  /// The index in StyledText::runs of the glyph's style, or kNoRun.
  std::size_t run = kNoRun;
};

/// A font at one size.
struct SizedFont {
  std::unique_ptr<hb_font_t, FreeShapingFont> font;
  float ascent = 0;
  float line_height = 0;
};

const Font* first_set(std::initializer_list<const Font*> fonts) {
  for (const Font* font : fonts) {
    if (font != nullptr) {
      return font;
    }
  }
  return nullptr;
}

/// Shapes parts of a text that have one style and one direction, and selects their fonts.
class Shaper {
 public:
  /// `layout->fonts` gets every font in use; the base regular font is its first entry.
  Shaper(const FontSet& fonts, TextLayout& layout)
      : fonts_(&fonts), layout_(&layout), buffer_(hb_buffer_create()) {
    (void)font_index(fonts.base.regular);
  }

  /// The font with index `font` in the layout, at `pixel_size`. Throws std::invalid_argument
  /// for a size outside the limits.
  const SizedFont& sized(std::uint16_t font, float pixel_size) {
    const int scale = subpixel_size(pixel_size);
    const auto [entry, added] = sized_.try_emplace(std::pair{font, scale});
    SizedFont& made = entry->second;
    if (added) {
      made.font.reset(hb_font_create(FontFaces::shaping(*layout_->fonts[font])));
      hb_font_set_scale(made.font.get(), scale, scale);
      hb_font_extents_t extents{};
      hb_font_get_h_extents(made.font.get(), &extents);
      made.ascent = static_cast<float>(extents.ascender) / kSubpixels;
      made.line_height =
          static_cast<float>(extents.ascender - extents.descender + extents.line_gap) / kSubpixels;
    }
    return made;
  }

  /// Shapes text[begin, end) in `style` at embedding level `level`, and appends its glyphs to
  /// `out` in logical order.
  void shape(std::string_view text, std::size_t begin, std::size_t end, const TextStyle& style,
             float pixel_size, std::uint8_t level, std::size_t run, std::vector<Shaped>& out) {
    const Font* primary = styled_font(style);
    // ponytail: the font is selected for each character alone. A mark or a variation selector
    // that the font of its base character does not have can go to a different font; select
    // fonts for each grapheme cluster if that shows.
    const Font* piece_font = primary;
    std::size_t piece_begin = begin;
    for (std::size_t i = begin; i < end;) {
      const Decoded character = decode_utf8(text.substr(0, end), i);
      const Font* font = font_for(character.code, primary, i == begin ? primary : piece_font);
      if (font != piece_font && i > piece_begin) {
        shape_piece(text, piece_begin, i, piece_font, pixel_size, level, run, out);
        piece_begin = i;
      }
      piece_font = font;
      i += character.length;
    }
    if (end > piece_begin) {
      shape_piece(text, piece_begin, end, piece_font, pixel_size, level, run, out);
    }
  }

 private:
  std::uint16_t font_index(const Font* font) {
    std::vector<const Font*>& used = layout_->fonts;
    const auto found = std::find(used.begin(), used.end(), font);
    if (found != used.end()) {
      return static_cast<std::uint16_t>(found - used.begin());
    }
    if (used.size() >= std::numeric_limits<std::uint16_t>::max()) {
      throw std::invalid_argument{"too many fonts in one text"};
    }
    used.push_back(font);
    return static_cast<std::uint16_t>(used.size() - 1);
  }

  /// The font of the style's family that is nearest to its weight and slant.
  [[nodiscard]] const Font* styled_font(const TextStyle& style) const {
    const FontFamily* family = &fonts_->base;
    if (const auto named = fonts_->families.find(style.font); named != fonts_->families.end()) {
      family = &named->second;
    }
    const Font* font = nullptr;
    if (style.bold && style.italic) {
      font = first_set({family->bold_italic, family->bold, family->italic});
    } else if (style.bold) {
      font = family->bold;
    } else if (style.italic) {
      font = family->italic;
    }
    return first_set({font, family->regular, fonts_->base.regular});
  }

  bool has(const Font* font, std::uint32_t code) {
    const auto [entry, added] = probes_.try_emplace(font);
    if (added) {
      entry->second.reset(hb_font_create(FontFaces::shaping(*font)));
    }
    hb_codepoint_t glyph = 0;
    return hb_font_get_nominal_glyph(entry->second.get(), code, &glyph) != 0;
  }

  /// `primary` if it has the character, else the first font of the fallback chain that has it,
  /// else `current` (the missing-character glyph does not change the font).
  const Font* font_for(std::uint32_t code, const Font* primary, const Font* current) {
    if (has(primary, code)) {
      return primary;
    }
    for (const Font* fallback : fonts_->fallbacks) {
      if (fallback != nullptr && has(fallback, code)) {
        return fallback;
      }
    }
    return current;
  }

  void shape_piece(std::string_view text, std::size_t begin, std::size_t end, const Font* font,
                   float pixel_size, std::uint8_t level, std::size_t run,
                   std::vector<Shaped>& out) {
    const std::uint16_t index = font_index(font);
    const SizedFont& sized_font = sized(index, pixel_size);
    const bool rtl = (level % 2) != 0;
    hb_buffer_clear_contents(buffer_.get());
    hb_buffer_add_utf8(buffer_.get(), text.data(), static_cast<int>(text.size()),
                       static_cast<unsigned>(begin), static_cast<int>(end - begin));
    // A fixed language, so the user's locale cannot change the shaping.
    hb_buffer_set_language(buffer_.get(), hb_language_from_string("und", -1));
    // ponytail: the script is guessed from the piece's first letter, so a piece with two
    // scripts in one direction is shaped as the first. Split pieces by script if that shows.
    hb_buffer_guess_segment_properties(buffer_.get());
    hb_buffer_set_direction(buffer_.get(), rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_shape(sized_font.font.get(), buffer_.get(), nullptr, 0);

    unsigned count = 0;
    const hb_glyph_info_t* first_info = hb_buffer_get_glyph_infos(buffer_.get(), &count);
    const hb_glyph_position_t* first_position =
        hb_buffer_get_glyph_positions(buffer_.get(), &count);
    const std::span<const hb_glyph_info_t> infos{first_info, count};
    const std::span<const hb_glyph_position_t> positions{first_position, count};
    out.reserve(out.size() + count);
    for (std::size_t i = 0; i < infos.size(); ++i) {
      // HarfBuzz gives right-to-left glyphs in visual order: the last character first.
      const std::size_t from = rtl ? infos.size() - 1 - i : i;
      out.push_back(Shaped{.glyph = infos[from].codepoint,
                           .cluster = infos[from].cluster,
                           .advance = static_cast<float>(positions[from].x_advance) / kSubpixels,
                           .x_offset = static_cast<float>(positions[from].x_offset) / kSubpixels,
                           .y_offset = static_cast<float>(positions[from].y_offset) / kSubpixels,
                           .pixel_size = pixel_size,
                           .ascent = sized_font.ascent,
                           .line_height = sized_font.line_height,
                           .font = index,
                           .level = level,
                           .run = run});
    }
  }

  const FontSet* fonts_ = nullptr;
  TextLayout* layout_ = nullptr;
  std::unique_ptr<hb_buffer_t, FreeBuffer> buffer_;
  std::map<std::pair<std::uint16_t, int>, SizedFont> sized_{};
  /// Fonts without a size, to ask which characters they have.
  std::map<const Font*, std::unique_ptr<hb_font_t, FreeShapingFont>> probes_{};
};

/// The ruby text of one `ruby` tag, shaped, and where its base is in the paragraph's glyphs.
struct Ruby {
  std::size_t first = 0;
  std::size_t last = 0;
  std::vector<Shaped> glyphs{};
  float width = 0;
  float ascent = 0;
  float line_height = 0;
};

/// Lays out the paragraphs of one text, line below line.
class Paragraphs {
 public:
  Paragraphs(const StyledText& styled, float pixel_size, float wrap_width, Shaper& shaper,
             TextLayout& layout)
      : styled_(&styled),
        text_(styled.text),
        pixel_size_(pixel_size),
        wrap_width_(wrap_width),
        shaper_(&shaper),
        layout_(&layout),
        breaks_(line_breaks(text_)),
        bidi_(bidi_levels(text_)),
        run_of_(text_.size(), kNoRun) {
    for (std::size_t run = 0; run < styled.runs.size(); ++run) {
      const std::size_t end = std::min(styled.runs[run].end, text_.size());
      for (std::size_t i = styled.runs[run].begin; i < end; ++i) {
        run_of_[i] = run;
      }
    }
  }

  void layout_all() {
    std::size_t start = 0;
    for (std::size_t i = 0; i < text_.size(); ++i) {
      if (breaks_[i] != LINEBREAK_MUSTBREAK && i + 1 < text_.size()) {
        continue;
      }
      // A paragraph ends after text[i]. Its line end is not shaped.
      std::size_t end = i + 1;
      while (end > start && (text_[end - 1] == '\n' || text_[end - 1] == '\r')) {
        --end;
      }
      paragraph(start, end);
      start = i + 1;
    }
  }

 private:
  [[nodiscard]] const TextStyle& style(std::size_t run) const {
    static const TextStyle kPlain{};
    return run == kNoRun ? kPlain : styled_->runs[run].style;
  }

  [[nodiscard]] bool is_space(const Shaped& glyph) const { return text_[glyph.cluster] == ' '; }

  /// Shapes text[start, end), a paragraph without its line end, and appends its lines.
  void paragraph(std::size_t start, std::size_t end) {
    std::vector<Shaped> glyphs;
    // One item for each stretch of one style and one direction.
    std::size_t item = start;
    for (std::size_t i = start; i <= end; ++i) {
      if (i < end && run_of_[i] == run_of_[item] && bidi_.level[i] == bidi_.level[item]) {
        continue;
      }
      if (i > item) {
        const TextStyle& item_style = style(run_of_[item]);
        shaper_->shape(text_, item, i, item_style,
                       item_style.size > 0 ? item_style.size : pixel_size_, bidi_.level[item],
                       run_of_[item], glyphs);
      }
      item = i;
    }
    const std::vector<Ruby> rubies = shape_rubies(glyphs);
    const std::uint8_t base_level = start < text_.size() ? bidi_.paragraph[start] : 0;

    // A glyph can start a line only if it starts a cluster (a ligature or a letter with its
    // marks stays whole), and not in the base text of a ruby.
    const auto starts_cluster = [&](std::size_t i) {
      return i == 0 || glyphs[i].cluster != glyphs[i - 1].cluster;
    };
    const auto may_break_before = [&](std::size_t i) {
      const std::size_t offset = glyphs[i].cluster;
      const bool in_ruby = glyphs[i].run == glyphs[i - 1].run && !style(glyphs[i].run).ruby.empty();
      return starts_cluster(i) && !in_ruby && offset > 0 &&
             breaks_[offset - 1] == LINEBREAK_ALLOWBREAK;
    };
    std::size_t line_start = 0;
    std::optional<std::size_t> break_at;  // The last place the current line may break.
    float width = 0;
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
      if (i > line_start && may_break_before(i)) {
        break_at = i;
      }
      while (i > line_start && !is_space(glyphs[i]) && width + glyphs[i].advance > wrap_width_) {
        if (!break_at && !starts_cluster(i)) {
          break;
        }
        // If no break is allowed in this line, split the word here.
        const std::size_t split = break_at.value_or(i);
        add_line(glyphs, line_start, split, rubies, base_level);
        line_start = split;
        break_at.reset();
        width = 0;
        for (std::size_t moved = line_start; moved < i; ++moved) {
          width += glyphs[moved].advance;
        }
      }
      width += glyphs[i].advance;
    }
    add_line(glyphs, line_start, glyphs.size(), rubies, base_level);
  }

  /// Shapes the ruby text of each ruby base in `glyphs`. A base that is narrower than its ruby
  /// gets half of the difference as space on each side.
  std::vector<Ruby> shape_rubies(std::vector<Shaped>& glyphs) {
    std::vector<Ruby> rubies;
    std::size_t last = 0;
    for (std::size_t first = 0; first < glyphs.size(); first = last) {
      const std::size_t run = glyphs[first].run;
      last = first + 1;
      while (last < glyphs.size() && glyphs[last].run == run) {
        ++last;
      }
      const TextStyle& base_style = style(run);
      if (base_style.ruby.empty()) {
        continue;
      }
      Ruby ruby{.first = first, .last = last};
      // ponytail: ruby text is one left-to-right line. Give it its own bidi pass if a game
      // needs right-to-left ruby.
      shaper_->shape(base_style.ruby, 0, base_style.ruby.size(), base_style,
                     glyphs[first].pixel_size * kRubyScale, 0, run, ruby.glyphs);
      for (const Shaped& glyph : ruby.glyphs) {
        ruby.width += glyph.advance;
        ruby.ascent = std::max(ruby.ascent, glyph.ascent);
        ruby.line_height = std::max(ruby.line_height, glyph.line_height);
      }
      float base_width = 0;
      for (std::size_t i = first; i < last; ++i) {
        base_width += glyphs[i].advance;
      }
      if (ruby.width > base_width) {
        const float space = (ruby.width - base_width) * kHalf;
        glyphs[first].x_offset += space;
        glyphs[first].advance += space;
        glyphs[last - 1].advance += space;
      }
      rubies.push_back(std::move(ruby));
    }
    return rubies;
  }

  /// The places of a line's glyphs from left to right, as indices into the line: the Unicode
  /// bidirectional algorithm's rules L1 (spaces at the line end) and L2 (reversal).
  [[nodiscard]] static std::vector<std::size_t> visual_order(std::span<const Shaped> line,
                                                             std::size_t content,
                                                             std::uint8_t base_level) {
    std::vector<int> levels;
    levels.reserve(line.size());
    for (const Shaped& glyph : line) {
      levels.push_back(levels.size() < content ? int{glyph.level} : int{base_level});
    }
    std::vector<std::size_t> order(line.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    const int highest = levels.empty() ? 0 : *std::max_element(levels.begin(), levels.end());
    for (int level = highest; level > 0; --level) {
      for (std::size_t i = 0; i < order.size();) {
        std::size_t end = i;
        while (end < order.size() && levels[order[end]] >= level) {
          ++end;
        }
        std::reverse(order.begin() + static_cast<std::ptrdiff_t>(i),
                     order.begin() + static_cast<std::ptrdiff_t>(end));
        i = std::max(end, i + 1);
      }
    }
    return order;
  }

  /// Appends glyphs [first, last) of a paragraph as one line.
  void add_line(std::span<const Shaped> glyphs, std::size_t first, std::size_t last,
                std::span<const Ruby> rubies, std::uint8_t base_level) {
    const std::span<const Shaped> shaped = glyphs.subspan(first, last - first);
    const SizedFont& base = shaper_->sized(0, pixel_size_);
    // An empty line is as high as the base font. Other lines make room for their glyphs.
    float ascent = shaped.empty() ? base.ascent : shaped.front().ascent;
    float below = shaped.empty() ? base.line_height - base.ascent
                                 : shaped.front().line_height - shaped.front().ascent;
    for (const Shaped& glyph : shaped) {
      ascent = std::max(ascent, glyph.ascent);
      below = std::max(below, glyph.line_height - glyph.ascent);
    }
    float ruby_height = 0;
    for (const Ruby& ruby : rubies) {
      if (ruby.first >= first && ruby.first < last) {
        ruby_height = std::max(ruby_height, ruby.line_height);
      }
    }
    TextLine line;
    line.top = top_;
    line.baseline = top_ + ruby_height + ascent;
    line.height = ruby_height + ascent + below;
    top_ += line.height;

    // Spaces at the line's end hang: they are not part of the width.
    std::size_t content = shaped.size();
    float hanging = 0;
    while (content > 0 && is_space(shaped[content - 1])) {
      --content;
      hanging += shaped[content].advance;
    }
    for (std::size_t i = 0; i < content; ++i) {
      line.width += shaped[i].advance;
    }

    // Where the pen is before each glyph, in logical order.
    std::vector<float> pens(shaped.size());
    line.glyphs.resize(shaped.size());
    // In a right-to-left paragraph the hanging spaces are at the left of the text.
    float pen = (base_level % 2) != 0 ? -hanging : 0;
    for (const std::size_t i : visual_order(shaped, content, base_level)) {
      const Shaped& glyph = shaped[i];
      const TextStyle& glyph_style = style(glyph.run);
      pens[i] = pen;
      line.glyphs[i] =
          LayoutGlyph{.glyph = glyph.glyph,
                      .text_offset = glyph.cluster,
                      .origin = {.x = pen + glyph.x_offset, .y = line.baseline - glyph.y_offset},
                      .font = glyph.font,
                      .rtl = (glyph.level % 2) != 0,
                      .pixel_size = glyph.pixel_size,
                      .advance = glyph.advance,
                      .color = glyph_style.color,
                      .reveal_pause = 0,
                      .reveal_speed = glyph_style.speed};
      pen += glyph.advance;
    }
    for (LayoutGlyph& glyph : line.glyphs) {
      while (next_pause_ < styled_->pauses.size() &&
             styled_->pauses[next_pause_].offset <= glyph.text_offset) {
        glyph.reveal_pause += styled_->pauses[next_pause_].seconds;
        ++next_pause_;
      }
    }

    // The part of the line that the glyphs [from, to) of the paragraph take, left and right.
    const auto extent = [&](std::size_t from, std::size_t to) {
      float left = std::numeric_limits<float>::max();
      float right = std::numeric_limits<float>::lowest();
      for (std::size_t i = from - first; i < to - first; ++i) {
        left = std::min(left, pens[i]);
        right = std::max(right, pens[i] + shaped[i].advance);
      }
      return std::pair{left, right};
    };
    for (const Ruby& ruby : rubies) {
      if (ruby.first < first || ruby.first >= last) {
        continue;
      }
      const auto [left, right] = extent(ruby.first, std::min(ruby.last, last));
      float x = ((left + right) * kHalf) - (ruby.width * kHalf);
      const float y = line.top + ruby_height - (ruby.line_height - ruby.ascent);
      RubyText text{.base_glyph = ruby.first - first, .glyphs = {}};
      text.glyphs.reserve(ruby.glyphs.size());
      for (const Shaped& glyph : ruby.glyphs) {
        text.glyphs.push_back(
            LayoutGlyph{.glyph = glyph.glyph,
                        .text_offset = glyphs[ruby.first].cluster,
                        .origin = {.x = x + glyph.x_offset, .y = y - glyph.y_offset},
                        .font = glyph.font,
                        .rtl = false,
                        .pixel_size = glyph.pixel_size,
                        .advance = glyph.advance,
                        .color = style(glyph.run).color,
                        .reveal_pause = 0,
                        .reveal_speed = 0});
        x += glyph.advance;
      }
      line.ruby.push_back(std::move(text));
    }
    // ponytail: one region for each run of a link on a line. In a line with two directions
    // the region can cover glyphs between the link's parts; split it by direction if needed.
    std::size_t run_end = first;
    for (std::size_t i = first; i < last; i = run_end) {
      run_end = i + 1;
      while (run_end < last && glyphs[run_end].run == glyphs[i].run) {
        ++run_end;
      }
      const std::string& link = style(glyphs[i].run).link;
      if (!link.empty()) {
        const auto [left, right] = extent(i, run_end);
        layout_->links.push_back(LinkRegion{
            .id = link,
            .rect = {.x = left, .y = line.top, .width = right - left, .height = line.height}});
      }
    }

    layout_->glyph_count += line.glyphs.size();
    layout_->lines.push_back(std::move(line));
  }

  const StyledText* styled_ = nullptr;
  std::string_view text_;
  float pixel_size_ = 0;
  float wrap_width_ = 0;
  Shaper* shaper_ = nullptr;
  TextLayout* layout_ = nullptr;
  std::vector<char> breaks_{};
  BidiLevels bidi_{};
  /// For each byte of the text, the index of its StyledRun, or kNoRun.
  std::vector<std::size_t> run_of_{};
  /// The top of the next line.
  float top_ = 0;
  /// The first pause that no glyph has yet.
  std::size_t next_pause_ = 0;
};

/// Renders `glyph` at the size set on `face`. Leaves the outputs empty for a glyph without
/// pixels, or one that cannot be rendered or extends over `max_extent` (1/64 pixel).
void rasterize(FT_Face face, std::uint32_t glyph, FT_Pos max_extent, GlyphRaster raster,
               AtlasGlyph& placement, std::vector<std::uint8_t>& coverage) {
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
  const bool distance = raster == GlyphRaster::kDistance;
  if (FT_Render_Glyph(slot, distance ? FT_RENDER_MODE_SDF : FT_RENDER_MODE_NORMAL) != 0) {
    return;
  }
  const FT_Bitmap& bitmap = slot->bitmap;
  const std::size_t width = bitmap.width;
  const std::size_t height = bitmap.rows;
  // A bitmap covers whole pixels, so it can be a pixel larger than the outline on each side. A
  // distance field also reaches kSdfSpread pixels out on each side.
  const std::size_t max_pixels = static_cast<std::size_t>(max_extent / kSubpixelsPerPixel) + 2 +
                                 (distance ? 2 * static_cast<std::size_t>(kSdfSpread) : 0);
  if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY || bitmap.buffer == nullptr || width == 0 ||
      height == 0 || width > max_pixels || height > max_pixels || bitmap.pitch < 0 ||
      static_cast<std::size_t>(bitmap.pitch) < width) {
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

/// Calls `visit` for every glyph of `layout`, ruby glyphs included.
template <typename Visit>
void for_each_glyph(const TextLayout& layout, Visit visit) {
  for (const TextLine& line : layout.lines) {
    for (const LayoutGlyph& glyph : line.glyphs) {
      visit(glyph);
    }
    for (const RubyText& ruby : line.ruby) {
      for (const LayoutGlyph& glyph : ruby.glyphs) {
        visit(glyph);
      }
    }
  }
}

float glyph_size(const TextLayout& layout, const LayoutGlyph& glyph) {
  return glyph.pixel_size > 0 ? glyph.pixel_size : layout.pixel_size;
}

}  // namespace

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
  FontSet fonts{};
  fonts.base.regular = &font;
  return layout_text(fonts, plain_text(text), pixel_size, wrap_width);
}

TextLayout layout_text(const FontSet& fonts, const StyledText& text, float pixel_size,
                       float wrap_width) {
  if (fonts.base.regular == nullptr) {
    throw std::invalid_argument{"FontSet::base.regular is not set"};
  }
  if (text.text.size() > static_cast<std::size_t>(INT_MAX)) {
    throw std::invalid_argument{"text is too long"};
  }
  TextLayout layout{};
  layout.pixel_size = pixel_size;
  Shaper shaper{fonts, layout};
  layout.line_height = shaper.sized(0, pixel_size).line_height;
  if (!text.text.empty()) {
    Paragraphs{text, pixel_size, wrap_width, shaper, layout}.layout_all();
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

std::size_t revealed_glyphs(const TextLayout& layout, double characters_per_second,
                            double elapsed_seconds) {
  if (!(characters_per_second > 0)) {
    return layout.glyph_count;
  }
  if (!(elapsed_seconds > 0)) {
    return 0;
  }
  // A stretch is a run of glyphs at one speed without a pause. Each stretch counts as the plain
  // reveal does, from the time it starts.
  double stretch_start = 0;
  double speed = characters_per_second;
  std::size_t in_stretch = 0;
  std::size_t shown = 0;
  for (const TextLine& line : layout.lines) {
    for (const LayoutGlyph& glyph : line.glyphs) {
      const double glyph_speed =
          glyph.reveal_speed > 0 ? glyph.reveal_speed : characters_per_second;
      const double pause = glyph.reveal_pause > 0 ? glyph.reveal_pause : 0;
      if (pause > 0 || glyph_speed != speed) {
        stretch_start += (static_cast<double>(in_stretch) / speed) + pause;
        speed = glyph_speed;
        in_stretch = 0;
      }
      if (std::floor(speed * (elapsed_seconds - stretch_start)) <
          static_cast<double>(in_stretch + 1)) {
        return shown;
      }
      ++in_stretch;
      ++shown;
    }
  }
  return shown;
}

PointF caret_position(const TextLayout& layout, std::size_t text_offset) {
  const TextLine* last_line = nullptr;
  for (const TextLine& line : layout.lines) {
    for (const LayoutGlyph& glyph : line.glyphs) {
      if (glyph.text_offset >= text_offset) {
        return {.x = glyph.origin.x + (glyph.rtl ? glyph.advance : 0), .y = line.baseline};
      }
    }
    if (!line.glyphs.empty()) {
      last_line = &line;
    }
  }
  if (last_line == nullptr) {
    return {.x = 0, .y = layout.lines.empty() ? 0 : layout.lines.back().baseline};
  }
  const LayoutGlyph& glyph = last_line->glyphs.back();
  return {.x = glyph.origin.x + (glyph.rtl ? 0 : glyph.advance), .y = last_line->baseline};
}

std::string_view link_at(const TextLayout& layout, PointF point) {
  for (const LinkRegion& link : layout.links) {
    const RectF& rect = link.rect;
    if (point.x >= rect.x && point.x < rect.x + rect.width && point.y >= rect.y &&
        point.y < rect.y + rect.height) {
      return link.id;
    }
  }
  return {};
}

GlyphAtlas::GlyphAtlas(GlyphRaster raster) : raster_(raster) { (void)pack(); }

GlyphAtlas::GlyphAtlas(const Font& font) : fonts_{&font} { (void)pack(); }

std::optional<GlyphAtlas::Key> GlyphAtlas::key_for(const TextLayout& layout,
                                                   const LayoutGlyph& glyph) const {
  const Font* font = nullptr;
  if (layout.fonts.empty()) {
    font = fonts_.empty() ? nullptr : fonts_.front();
  } else if (glyph.font < layout.fonts.size()) {
    font = layout.fonts[glyph.font];
  }
  const auto found = std::find(fonts_.begin(), fonts_.end(), font);
  const float size = glyph_size(layout, glyph) * scale_;
  if (font == nullptr || found == fonts_.end() || !(size > 0 && size <= kMaxFontPixelSize)) {
    return std::nullopt;
  }
  return Key{.font = static_cast<std::size_t>(found - fonts_.begin()),
             .size = subpixel_size(size),
             .glyph = glyph.glyph};
}

bool GlyphAtlas::prepare(const TextLayout& layout, float scale) {
  const float new_size = layout.pixel_size * scale;
  // Check every size before anything changes.
  (void)subpixel_size(new_size);
  for_each_glyph(layout, [&](const LayoutGlyph& glyph) {
    (void)subpixel_size(glyph_size(layout, glyph) * scale);
  });
  bool changed = false;
  if (new_size != pixel_size_) {
    glyphs_.clear();
    pixel_size_ = new_size;
    changed = true;
  }
  scale_ = scale;
  for (const Font* font : layout.fonts) {
    if (font != nullptr && std::find(fonts_.begin(), fonts_.end(), font) == fonts_.end()) {
      fonts_.push_back(font);
    }
  }

  // The font and size that FreeType is set to.
  std::optional<Key> set{};
  bool cannot_draw = false;
  for_each_glyph(layout, [&](const LayoutGlyph& glyph) {
    const std::optional<Key> key = key_for(layout, glyph);
    if (!key || cannot_draw) {
      return;
    }
    const auto [entry, added] = glyphs_.try_emplace(*key);
    if (!added) {
      return;
    }
    changed = true;
    const FT_Face face = FontFaces::face(*fonts_[key->font]);
    if (!set || set->font != key->font || set->size != key->size) {
      if (FT_Set_Char_Size(face, 0, key->size, 0, 0) != 0) {
        cannot_draw = true;
        return;
      }
      set = key;
    }
    rasterize(face, key->glyph, FT_Pos{key->size} * kMaxGlyphEms, raster_, entry->second.placement,
              entry->second.coverage);
  });
  if (cannot_draw) {
    glyphs_.clear();
    (void)pack();
    throw FontError{"the font cannot be drawn at " + std::to_string(new_size) + " pixels"};
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
  if (fonts_.empty() || !(pixel_size_ > 0)) {
    return nullptr;
  }
  const auto found =
      glyphs_.find(Key{.font = 0, .size = subpixel_size(pixel_size_), .glyph = glyph});
  return found == glyphs_.end() ? nullptr : &found->second.placement;
}

const AtlasGlyph* GlyphAtlas::find(const TextLayout& layout, const LayoutGlyph& glyph) const {
  const std::optional<Key> key = key_for(layout, glyph);
  if (!key) {
    return nullptr;
  }
  const auto found = glyphs_.find(*key);
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

  Image packed{};
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
  std::vector<Quad> quads{};
  if (!(layout.pixel_size > 0)) {
    return quads;
  }
  quads.reserve(std::min(visible_glyphs, layout.glyph_count));
  const float scale = atlas.pixel_size() / layout.pixel_size;
  const auto atlas_width = static_cast<float>(atlas.image().size.width);
  const auto atlas_height = static_cast<float>(atlas.image().size.height);
  const auto add = [&](const LayoutGlyph& glyph) {
    const AtlasGlyph* placed = atlas.find(layout, glyph);
    if (placed == nullptr || placed->width == 0) {
      return;
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
                       glyph.color.value_or(tint)));
  };
  std::size_t remaining = visible_glyphs;
  for (const TextLine& line : layout.lines) {
    for (std::size_t i = 0; i < line.glyphs.size(); ++i) {
      if (remaining == 0) {
        return quads;
      }
      --remaining;
      add(line.glyphs[i]);
      for (const RubyText& ruby : line.ruby) {
        if (ruby.base_glyph != i) {
          continue;
        }
        for (const LayoutGlyph& glyph : ruby.glyphs) {
          add(glyph);
        }
      }
    }
  }
  return quads;
}

ShaderBytecode sdf_text_shader() { return shaders::sdf_text_frag(); }

SdfTextUniforms sdf_text_uniforms(Color outline_color, float outline_width) {
  // FreeType's 8-bit distance fields give kSdfSpread pixels 128 steps on each side of the edge.
  constexpr float kLevelsPerSide = 128;
  return {.outline_color = {outline_color.r, outline_color.g, outline_color.b, outline_color.a},
          .outline_width = std::clamp(outline_width, 0.0F, static_cast<float>(kSdfSpread - 1)),
          .pixels_per_level = static_cast<float>(kSdfSpread) / kLevelsPerSide,
          .unused_padding = {}};
}

}  // namespace vnmaki
