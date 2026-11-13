#include "engine/dialogue/say_box.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "engine/text/markup.hpp"
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

SayBoxView::SayBoxView(IRenderer& renderer, const Font& font, SayBoxStyle style)
    : SayBoxView(renderer, FontSet{.base = {.regular = &font}}, style) {}

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
  text.markup = true;
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

SayBoxView::SayBoxView(IRenderer& renderer, FontSet fonts, SayBoxStyle style)
    : fonts_(fonts), style_(style), ui_(renderer, std::move(fonts)) {}

namespace {

UiNode* find_reveal(UiNode& node) {
  if (node.reveal) {
    return &node;
  }
  for (UiNode& child : node.children) {
    if (UiNode* found = find_reveal(child)) {
      return found;
    }
  }
  return nullptr;
}

}  // namespace

void SayBoxView::set_screen_source(ScreenSource source, Size canvas) {
  source_ = std::move(source);
  canvas_ = canvas;
  built_for_.reset();
  built_.reset();
  has_layout_ = false;
}

void SayBoxView::set_text_scale(float scale) {
  text_scale_ = scale;
  built_for_.reset();
  built_.reset();
  has_layout_ = false;
}

void SayBoxView::set_fonts(FontSet fonts) {
  fonts_ = fonts;
  ui_.set_fonts(std::move(fonts));
  built_for_.reset();
  built_.reset();
  has_layout_ = false;
}

// The script's tree for `line`, laid out; null when there is no source, it gave none, or the tree
// has no node to reveal.
UiNode* SayBoxView::scripted_screen(const DialogueLine& line) {
  if (!source_) {
    return nullptr;
  }
  if (!built_for_ || *built_for_ != line) {
    built_for_ = line;
    built_ = source_(line);
    if (built_ && find_reveal(*built_) == nullptr) {
      built_.reset();
    }
    if (built_) {
      scale_text_sizes(*built_, text_scale_);
      layout_tree(*built_, canvas_, ui_.text_measure());
    }
    has_layout_ = false;
  }
  return built_ ? &*built_ : nullptr;
}

void SayBoxView::layout(const DialogueLine& line) {
  UiNode* scripted = scripted_screen(line);
  if (has_layout_ && line.text == laid_out_text_) {
    return;
  }
  float text_size = style_.text_size * text_scale_;
  float wrap_width = style_.wrap_width;
  if (scripted != nullptr) {
    const UiNode& reveal = *find_reveal(*scripted);
    text_size = reveal.style.text_size;
    wrap_width = reveal.style.width.value_or(reveal.rect.width);
  }
  StyledText styled;
  try {
    styled = parse_markup(line.text);
  } catch (const MarkupError&) {
    styled = plain_text(line.text);  // The runtime rejects bad tags; this is for other callers.
  }
  text_layout_ = layout_text(fonts_, styled, text_size, wrap_width);
  laid_out_text_ = line.text;
  has_layout_ = true;
}

UiNode SayBoxView::laid_out_screen(const DialogueLine& line,
                                   std::optional<std::size_t> visible_glyphs) {
  UiNode screen = make_say_screen(style_, line, visible_glyphs);
  scale_text_sizes(screen, text_scale_);
  const Size canvas{.width = static_cast<int>(std::ceil(style_.box.x + style_.box.width)),
                    .height = static_cast<int>(std::ceil(style_.box.y + style_.box.height))};
  layout_tree(screen, canvas, ui_.text_measure());
  return screen;
}

std::size_t SayBoxView::glyph_count(const DialogueLine& line) {
  layout(line);
  return text_layout_.glyph_count;
}

std::size_t SayBoxView::revealed(const DialogueLine& line, double characters_per_second,
                                 double elapsed_seconds) {
  layout(line);
  return revealed_glyphs(text_layout_, characters_per_second, elapsed_seconds);
}

std::string SayBoxView::link_at(const DialogueLine& line, PointF point) {
  layout(line);
  PointF origin{.x = style_.box.x + style_.text_offset.x, .y = style_.box.y + style_.text_offset.y};
  if (UiNode* scripted = scripted_screen(line)) {
    const UiNode& reveal = *find_reveal(*scripted);
    origin = {.x = reveal.rect.x + reveal.style.padding, .y = reveal.rect.y + reveal.style.padding};
  }
  return std::string{
      vnmaki::link_at(text_layout_, {.x = point.x - origin.x, .y = point.y - origin.y})};
}

void SayBoxView::prepare(const DialogueLine& line, float scale) {
  if (const UiNode* scripted = scripted_screen(line)) {
    ui_.prepare(*scripted, scale);
    return;
  }
  ui_.prepare(laid_out_screen(line, std::nullopt), scale);
}

void SayBoxView::draw(const DialogueLine& line, std::size_t visible_glyphs, float scale,
                      PointF origin) {
  if (UiNode* scripted = scripted_screen(line)) {
    find_reveal(*scripted)->visible_glyphs = visible_glyphs;
    ui_.draw(*scripted, scale, origin);
    return;
  }
  ui_.draw(laid_out_screen(line, visible_glyphs), scale, origin);
}

}  // namespace vnmaki
