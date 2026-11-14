#include "engine/ui/ui_renderer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "engine/render/image.hpp"
#include "engine/text/markup.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki {

namespace {

// Layouts are kept between frames; past this many the cache starts again.
constexpr std::size_t kMaxCachedLayouts = 256;
constexpr float kMinWrapWidth = 1;
const Color kHollowMark = rgba8(16, 18, 36);  // the inside of an unchecked toggle's mark
constexpr float kPlaceholderAlpha = 0.5F;     // an input's placeholder is this opaque
constexpr float kCaretWidthEm = 0.08F;        // an input's caret, as a share of the text size

bool shows_text(const UiNode& node) {
  return (node.kind == NodeKind::kText || node.kind == NodeKind::kButton ||
          node.kind == NodeKind::kToggle || node.kind == NodeKind::kInput) &&
         !display_text(node).empty();
}

// The value of a state-dependent field: `focus` while the node is focused (or hovered and has no
// `hover`), `hover` while it is hovered, else `base`. Only a node that can take the focus changes.
std::optional<Color> by_state(const UiNode& node, const std::optional<Color>& base,
                              const std::optional<Color>& hover,
                              const std::optional<Color>& focus) {
  const bool focused = node.state.focused;
  const bool hovered = node.state.hovered;
  if (!is_focusable(node)) {
    return base;
  }
  if (focused && focus) {
    return focus;
  }
  if (hovered && hover) {
    return hover;
  }
  return (focused || hovered) && focus ? focus : base;
}

std::optional<Color> text_color(const UiNode& node) {
  return by_state(node, node.style.color, node.style.hover_color, node.style.focus_color);
}

float content_width(const UiNode& node) {
  return std::max(node.rect.width - (2 * node.style.padding), kMinWrapWidth);
}

// The wrap limit of a text node's content: its width, or for vertical text its height.
float content_limit(const UiNode& node) {
  const float length = node.style.vertical ? node.rect.height : node.rect.width;
  return std::max(length - (2 * node.style.padding), kMinWrapWidth);
}

float widest_line(const TextLayout& layout) {
  float widest = 0;
  for (const TextLine& line : layout.lines) {
    widest = std::max(widest, line.width);
  }
  return widest;
}

}  // namespace

UiRenderer::UiRenderer(IRenderer& renderer, const Font& font, const Vfs* vfs)
    : UiRenderer(renderer, FontSet{.base = {.regular = &font}}, vfs) {}

UiRenderer::UiRenderer(IRenderer& renderer, FontSet fonts, const Vfs* vfs)
    : renderer_(&renderer),
      fonts_(std::move(fonts)),
      vfs_(vfs),
      white_(renderer.create_texture(Image{.size = {.width = 1, .height = 1},
                                           .pixels = {UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX}},
                                     TextureFilter::kNearest)) {}

UiRenderer::~UiRenderer() {
  renderer_->destroy_texture(white_);
  for (const auto& [size, entry] : atlases_) {
    if (entry.texture) {
      renderer_->destroy_texture(*entry.texture);
    }
  }
  for (const auto& [name, texture] : images_) {
    if (texture) {
      renderer_->destroy_texture(*texture);
    }
  }
}

void UiRenderer::set_fonts(FontSet fonts) {
  fonts_ = std::move(fonts);
  layouts_.clear();
}

const TextLayout& UiRenderer::layout(const UiNode& node, float wrap_width) {
  auto key = std::make_tuple(display_text(node), node.markup, node.style.text_size, wrap_width,
                             node.style.font, node.style.vertical);
  const auto found = layouts_.find(key);
  if (found != layouts_.end()) {
    return found->second;
  }
  if (layouts_.size() >= kMaxCachedLayouts) {
    layouts_.clear();
  }
  StyledText styled;
  try {
    styled = node.markup ? parse_markup(node.text) : plain_text(display_text(node));
  } catch (const MarkupError&) {
    styled = plain_text(node.text);
  }
  if (!node.style.font.empty()) {
    for (StyledRun& run : styled.runs) {
      if (run.style.font.empty()) {
        run.style.font = node.style.font;
      }
    }
  }
  TextLayout laid_out =
      layout_text(fonts_, styled, node.style.text_size, wrap_width,
                  node.style.vertical ? TextOrientation::kVertical : TextOrientation::kHorizontal);
  return layouts_.emplace(std::move(key), std::move(laid_out)).first->second;
}

TextMeasure UiRenderer::text_measure() {
  return [this](const UiNode& node, float max_width) {
    if (display_text(node).empty()) {
      return MeasuredSize{};
    }
    const TextLayout& text = layout(node, std::max(max_width, kMinWrapWidth));
    const PointF extent = layout_extent(text);
    return MeasuredSize{.width = extent.x, .height = extent.y};
  };
}

std::optional<TextureId> UiRenderer::image_texture(const std::string& name) {
  const auto found = images_.find(name);
  if (found != images_.end()) {
    return found->second;
  }
  std::optional<TextureId> texture;
  if (name.starts_with(kGeneratedImagePrefix)) {
    const std::optional<Image> generated = generated_ ? generated_(name) : std::nullopt;
    if (generated) {
      texture = renderer_->create_texture(*generated, TextureFilter::kLinear);
    }
  } else if (vfs_ != nullptr) {
    std::string path = "images/" + name + ".png";
    for (const char* extension : {".jpg", ".webp"}) {
      if (!vfs_->exists(path)) {
        path = "images/" + name + extension;
      }
    }
    const std::optional<std::vector<std::byte>> bytes = vfs_->read(path);
    if (bytes) {
      try {
        texture = renderer_->create_texture(decode_image(*bytes), TextureFilter::kLinear);
      } catch (const ImageError&) {
        texture.reset();  // an image that cannot be read is not drawn
      }
    }
  }
  images_.emplace(name, texture);
  return texture;
}

void UiRenderer::prepare(const UiNode& root, float scale) {
  if (shows_text(root)) {
    const TextLayout& text = layout(root, content_limit(root));
    SizeAtlas& entry = atlases_[root.style.text_size];
    if (text.glyph_count > 0 && (entry.atlas.prepare(text, scale) || !entry.texture)) {
      if (entry.texture) {
        renderer_->destroy_texture(*entry.texture);
        entry.texture.reset();
      }
      entry.texture = renderer_->create_texture(entry.atlas.image(), TextureFilter::kNearest);
    }
  }
  if (root.kind == NodeKind::kImage) {
    static_cast<void>(image_texture(root.text));
  }
  for (const UiNode& child : root.children) {
    prepare(child, scale);
  }
}

void UiRenderer::draw_fill(RectF rect, Color color) {
  renderer_->draw(std::vector<Quad>{quad_from_rect(rect, kWholeTexture, color)},
                  {.texture = white_});
}

void UiRenderer::draw_text(const UiNode& node, float scale, PointF origin) {
  const TextLayout& text = layout(node, content_limit(node));
  const SizeAtlas& entry = atlases_[node.style.text_size];
  if (text.glyph_count == 0 || !entry.texture) {
    return;
  }
  float indent = 0;
  if (node.kind == NodeKind::kButton) {
    indent = std::max((content_width(node) - widest_line(text)) / 2, 0.0F);  // centred caption
  } else if (node.kind == NodeKind::kToggle) {
    indent = node.style.text_size * kToggleMarkEm;  // room for the mark
  } else if (node.style.vertical) {
    indent = content_width(node) - layout_extent(text).x;  // the first column is at the right edge
  }
  const PointF at{.x = origin.x + ((node.rect.x + node.style.padding + indent) * scale),
                  .y = origin.y + ((node.rect.y + node.style.padding) * scale)};
  const std::size_t visible = node.visible_glyphs.value_or(text.glyph_count);
  Color tint = *text_color(node);
  if (node.kind == NodeKind::kInput && node.text.empty()) {
    tint.a *= kPlaceholderAlpha;
  }
  const std::vector<Quad> quads = text_quads(text, entry.atlas, at, visible, tint);
  if (!quads.empty()) {
    renderer_->draw(quads, {.texture = *entry.texture});
  }
}

// The square before a toggle's caption: filled with the node's `fill` colour when on, a hollow
// frame when off.
void UiRenderer::draw_toggle_mark(const UiNode& node, float scale, PointF origin) {
  constexpr float kFrameShare = 0.12F;  // frame width, as a share of the mark
  const float size = node.style.text_size * scale;
  const RectF mark{.x = origin.x + ((node.rect.x + node.style.padding) * scale),
                   .y = origin.y + ((node.rect.y + node.style.padding) * scale),
                   .width = size,
                   .height = size};
  draw_fill(mark, node.style.fill);
  if (!node.checked) {
    const float frame = size * kFrameShare;
    draw_fill({.x = mark.x + frame,
               .y = mark.y + frame,
               .width = size - (2 * frame),
               .height = size - (2 * frame)},
              kHollowMark);
  }
}

// A frame of `width` pixels inside `rect`.
void UiRenderer::draw_border(const UiNode& node, RectF rect, float width) {
  const Color color = by_state(node, node.style.border_color, node.style.hover_border_color,
                               node.style.focus_border_color)
                          .value_or(node.style.color);
  const float side = std::min(width, std::min(rect.width, rect.height) / 2);
  draw_fill({.x = rect.x, .y = rect.y, .width = rect.width, .height = side}, color);
  draw_fill({.x = rect.x, .y = rect.y + rect.height - side, .width = rect.width, .height = side},
            color);
  draw_fill({.x = rect.x, .y = rect.y + side, .width = side, .height = rect.height - (2 * side)},
            color);
  draw_fill({.x = rect.x + rect.width - side,
             .y = rect.y + side,
             .width = side,
             .height = rect.height - (2 * side)},
            color);
}

// The bar after the last character of a focused input (the text always grows at its end).
void UiRenderer::draw_caret(const UiNode& node, float scale, PointF origin) {
  const TextLayout& text = layout(node, content_width(node));
  float x = 0;
  float top = 0;
  float height = node.style.text_size;
  if (!node.text.empty() && !text.lines.empty()) {
    x = text.lines.back().width;
    top = text.lines.back().top;
    height = text.lines.back().height;
  }
  draw_fill({.x = origin.x + ((node.rect.x + node.style.padding + x) * scale),
             .y = origin.y + ((node.rect.y + node.style.padding + top) * scale),
             .width = std::max(node.style.text_size * kCaretWidthEm * scale, 1.0F),
             .height = height * scale},
            *text_color(node));
}

void UiRenderer::draw_node(const UiNode& node, float scale, PointF origin) {
  const RectF rect{.x = origin.x + (node.rect.x * scale),
                   .y = origin.y + (node.rect.y * scale),
                   .width = node.rect.width * scale,
                   .height = node.rect.height * scale};
  const std::optional<Color> fill = by_state(
      node, node.style.background, node.style.hover_background, node.style.focus_background);
  if (fill) {
    draw_fill(rect, *fill);
  }
  if (node.style.border_width > 0) {
    draw_border(node, rect, node.style.border_width * scale);
  }
  if (node.kind == NodeKind::kToggle) {
    draw_toggle_mark(node, scale, origin);
  } else if (node.kind == NodeKind::kSlider || node.kind == NodeKind::kBar) {
    const double span = node.max - node.min;
    const auto share = static_cast<float>(std::clamp((node.value - node.min) / span, 0.0, 1.0));
    draw_fill({.x = rect.x, .y = rect.y, .width = rect.width * share, .height = rect.height},
              node.style.fill);
  }
  if (shows_text(node)) {
    draw_text(node, scale, origin);
  }
  if (node.kind == NodeKind::kInput && node.state.focused) {
    draw_caret(node, scale, origin);
  }
  if (node.kind == NodeKind::kImage) {
    if (const std::optional<TextureId> texture = image_texture(node.text)) {
      renderer_->draw(std::vector<Quad>{quad_from_rect(rect, kWholeTexture, node.style.color)},
                      {.texture = *texture});
    }
  }
  for (const UiNode& child : node.children) {
    draw_node(child, scale, origin);
  }
}

void UiRenderer::draw(const UiNode& root, float scale, PointF origin) {
  prepare(root, scale);
  draw_node(root, scale, origin);
}

}  // namespace vnmaki
