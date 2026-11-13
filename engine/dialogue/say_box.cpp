#include "engine/dialogue/say_box.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "engine/ui/ui_layout.hpp"

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

UiNode make_say_screen(const SayBoxStyle& style, const DialogueLine& line,
                       std::optional<std::size_t> visible_glyphs) {
  UiNode name = make_node(NodeKind::kText);
  name.text = line.speaker;
  name.label = line.speaker;
  name.style.position = style.name_offset;
  name.style.text_size = style.name_size;
  name.style.color = line.color ? color_from_rgb(*line.color) : style.text_color;

  UiNode text = make_node(NodeKind::kText);
  text.text = line.text;
  text.label = line.text;
  text.style.position = style.text_offset;
  text.style.width = style.wrap_width;
  text.style.text_size = style.text_size;
  text.style.color = style.text_color;
  text.visible_glyphs = visible_glyphs;

  UiNode panel = make_node(NodeKind::kBox);
  panel.style.position = PointF{.x = style.box.x, .y = style.box.y};
  panel.style.width = style.box.width;
  panel.style.height = style.box.height;
  panel.style.background = style.box_color;
  panel.children.push_back(std::move(name));
  panel.children.push_back(std::move(text));

  UiNode root = make_node(NodeKind::kBox);
  root.children.push_back(std::move(panel));
  return root;
}

SayBoxView::SayBoxView(IRenderer& renderer, const Font& font, SayBoxStyle style)
    : font_(&font), style_(style), ui_(renderer, font) {}

UiNode SayBoxView::laid_out_screen(const DialogueLine& line,
                                   std::optional<std::size_t> visible_glyphs) {
  UiNode screen = make_say_screen(style_, line, visible_glyphs);
  const Size canvas{.width = static_cast<int>(std::ceil(style_.box.x + style_.box.width)),
                    .height = static_cast<int>(std::ceil(style_.box.y + style_.box.height))};
  layout_tree(screen, canvas, ui_.text_measure());
  return screen;
}

std::size_t SayBoxView::glyph_count(const DialogueLine& line) {
  if (!has_layout_ || line.text != laid_out_text_) {
    text_layout_ = layout_text(*font_, line.text, style_.text_size, style_.wrap_width);
    laid_out_text_ = line.text;
    has_layout_ = true;
  }
  return text_layout_.glyph_count;
}

void SayBoxView::prepare(const DialogueLine& line, float scale) {
  ui_.prepare(laid_out_screen(line, std::nullopt), scale);
}

void SayBoxView::draw(const DialogueLine& line, std::size_t visible_glyphs, float scale,
                      PointF origin) {
  ui_.draw(laid_out_screen(line, visible_glyphs), scale, origin);
}

}  // namespace vnmaki
