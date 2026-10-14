#include "engine/dialogue/say_box.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "engine/render/image.hpp"

namespace vnmaki {

namespace {

constexpr int kMarginDivisor = 16;    // box margin: width / 16
constexpr int kPaddingDivisor = 64;   // padding: width / 64
constexpr int kTextSizeDivisor = 24;  // text size: height / 24
constexpr int kNameSizeDivisor = 32;  // name size: height / 32
constexpr int kBoxHeightNumerator = 7;
constexpr int kBoxHeightDenominator = 24;  // box height: height * 7 / 24
constexpr int kBottomGapDivisor = 24;      // gap under the box: height / 24
constexpr int kNamePaddingDivisor = 2;     // gap between name and line: padding / 2
constexpr std::uint8_t kBoxRed = 16;
constexpr std::uint8_t kBoxGreen = 16;
constexpr std::uint8_t kBoxBlue = 32;
constexpr std::uint8_t kBoxAlpha = 204;

constexpr int kRedShift = 16;
constexpr int kGreenShift = 8;
constexpr std::uint32_t kByteMask = 0xFF;

Color color_from_rgb(std::uint32_t rgb) {
  return rgba8(static_cast<std::uint8_t>((rgb >> kRedShift) & kByteMask),
               static_cast<std::uint8_t>((rgb >> kGreenShift) & kByteMask),
               static_cast<std::uint8_t>(rgb & kByteMask));
}

}  // namespace

SayBoxStyle SayBoxStyle::for_resolution(Size virtual_size) {
  const int margin = virtual_size.width / kMarginDivisor;
  const int padding = virtual_size.width / kPaddingDivisor;
  const int name_size = virtual_size.height / kNameSizeDivisor;
  const int text_size = virtual_size.height / kTextSizeDivisor;
  const int name_gap = padding / kNamePaddingDivisor;
  const int box_height = virtual_size.height * kBoxHeightNumerator / kBoxHeightDenominator;
  const int box_width = virtual_size.width - (2 * margin);
  const int box_top = virtual_size.height - (virtual_size.height / kBottomGapDivisor) - box_height;
  return SayBoxStyle{
      .box = {.x = static_cast<float>(margin),
              .y = static_cast<float>(box_top),
              .width = static_cast<float>(box_width),
              .height = static_cast<float>(box_height)},
      .box_color = rgba8(kBoxRed, kBoxGreen, kBoxBlue, kBoxAlpha),
      .name_offset = {.x = static_cast<float>(padding), .y = static_cast<float>(padding)},
      .text_offset = {.x = static_cast<float>(padding),
                      .y = static_cast<float>(padding + name_size + name_gap)},
      .name_size = static_cast<float>(name_size),
      .text_size = static_cast<float>(text_size),
      .wrap_width = static_cast<float>(box_width - (2 * padding)),
      .text_color = kWhite,
  };
}

SayBoxView::SayBoxView(IRenderer& renderer, const Font& font, SayBoxStyle style)
    : renderer_(&renderer),
      font_(&font),
      style_(style),
      white_(renderer.create_texture(Image{.size = {.width = 1, .height = 1},
                                           .pixels = {UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX}},
                                     TextureFilter::kNearest)),
      name_atlas_(font),
      text_atlas_(font) {}

SayBoxView::~SayBoxView() {
  renderer_->destroy_texture(white_);
  if (name_texture_) {
    renderer_->destroy_texture(*name_texture_);
  }
  if (text_texture_) {
    renderer_->destroy_texture(*text_texture_);
  }
}

void SayBoxView::layout(const DialogueLine& line) {
  if (has_layout_ && line.speaker == laid_out_speaker_ && line.text == laid_out_text_) {
    return;
  }
  name_layout_ = layout_text(*font_, line.speaker, style_.name_size, kNoWrap);
  text_layout_ = layout_text(*font_, line.text, style_.text_size, style_.wrap_width);
  laid_out_speaker_ = line.speaker;
  laid_out_text_ = line.text;
  has_layout_ = true;
}

std::size_t SayBoxView::glyph_count(const DialogueLine& line) {
  layout(line);
  return text_layout_.glyph_count;
}

void SayBoxView::upload(const GlyphAtlas& atlas, std::optional<TextureId>& texture) {
  if (texture) {
    renderer_->destroy_texture(*texture);
    texture.reset();
  }
  texture = renderer_->create_texture(atlas.image(), TextureFilter::kNearest);
}

void SayBoxView::prepare_text(const TextLayout& text, GlyphAtlas& atlas,
                              std::optional<TextureId>& texture, float scale) {
  if (text.glyph_count == 0) {
    return;  // An empty atlas image cannot be a texture.
  }
  if (atlas.prepare(text, scale) || !texture) {
    upload(atlas, texture);
  }
}

void SayBoxView::draw_text(const TextLayout& text, const GlyphAtlas& atlas,
                           const std::optional<TextureId>& texture, PointF origin,
                           std::size_t visible_glyphs, Color tint) {
  if (text.glyph_count == 0 || !texture) {
    return;
  }
  const std::vector<Quad> quads = text_quads(text, atlas, origin, visible_glyphs, tint);
  if (!quads.empty()) {
    renderer_->draw(quads, {.texture = *texture});
  }
}

void SayBoxView::prepare(const DialogueLine& line, float scale) {
  layout(line);
  prepare_text(name_layout_, name_atlas_, name_texture_, scale);
  prepare_text(text_layout_, text_atlas_, text_texture_, scale);
}

void SayBoxView::draw(const DialogueLine& line, std::size_t visible_glyphs, float scale,
                      PointF origin) {
  prepare(line, scale);
  const auto place = [&](PointF offset) {
    return PointF{.x = origin.x + ((style_.box.x + offset.x) * scale),
                  .y = origin.y + ((style_.box.y + offset.y) * scale)};
  };
  const RectF box{.x = place({}).x,
                  .y = place({}).y,
                  .width = style_.box.width * scale,
                  .height = style_.box.height * scale};
  renderer_->draw(std::vector<Quad>{quad_from_rect(box, kWholeTexture, style_.box_color)},
                  {.texture = white_});
  const Color name_color = line.color ? color_from_rgb(*line.color) : style_.text_color;
  draw_text(name_layout_, name_atlas_, name_texture_, place(style_.name_offset),
            name_layout_.glyph_count, name_color);
  draw_text(text_layout_, text_atlas_, text_texture_, place(style_.text_offset), visible_glyphs,
            style_.text_color);
}

}  // namespace vnmaki
