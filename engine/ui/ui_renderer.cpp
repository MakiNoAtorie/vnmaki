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

bool shows_text(const UiNode& node) {
  return (node.kind == NodeKind::kText || node.kind == NodeKind::kButton ||
          node.kind == NodeKind::kToggle) &&
         !node.text.empty();
}

float content_width(const UiNode& node) {
  return std::max(node.rect.width - (2 * node.style.padding), kMinWrapWidth);
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

const TextLayout& UiRenderer::layout(const UiNode& node, float wrap_width) {
  auto key = std::make_tuple(node.text, node.markup, node.style.text_size, wrap_width);
  const auto found = layouts_.find(key);
  if (found != layouts_.end()) {
    return found->second;
  }
  if (layouts_.size() >= kMaxCachedLayouts) {
    layouts_.clear();
  }
  StyledText styled;
  try {
    styled = node.markup ? parse_markup(node.text) : plain_text(node.text);
  } catch (const MarkupError&) {
    styled = plain_text(node.text);
  }
  TextLayout laid_out = layout_text(fonts_, styled, node.style.text_size, wrap_width);
  return layouts_.emplace(std::move(key), std::move(laid_out)).first->second;
}

TextMeasure UiRenderer::text_measure() {
  return [this](const UiNode& node, float max_width) {
    if (node.text.empty()) {
      return MeasuredSize{};
    }
    const TextLayout& text = layout(node, std::max(max_width, kMinWrapWidth));
    const float height = text.lines.empty() ? 0 : text.lines.back().top + text.lines.back().height;
    return MeasuredSize{.width = widest_line(text), .height = height};
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
    if (!vfs_->exists(path)) {
      path = "images/" + name + ".jpg";
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
    const TextLayout& text = layout(root, content_width(root));
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
  const TextLayout& text = layout(node, content_width(node));
  const SizeAtlas& entry = atlases_[node.style.text_size];
  if (text.glyph_count == 0 || !entry.texture) {
    return;
  }
  float indent = 0;
  if (node.kind == NodeKind::kButton) {
    indent = std::max((content_width(node) - widest_line(text)) / 2, 0.0F);  // centred caption
  } else if (node.kind == NodeKind::kToggle) {
    indent = node.style.text_size * kToggleMarkEm;  // room for the mark
  }
  const PointF at{.x = origin.x + ((node.rect.x + node.style.padding + indent) * scale),
                  .y = origin.y + ((node.rect.y + node.style.padding) * scale)};
  const std::size_t visible = node.visible_glyphs.value_or(text.glyph_count);
  const std::vector<Quad> quads = text_quads(text, entry.atlas, at, visible, node.style.color);
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

void UiRenderer::draw_node(const UiNode& node, float scale, PointF origin) {
  const RectF rect{.x = origin.x + (node.rect.x * scale),
                   .y = origin.y + (node.rect.y * scale),
                   .width = node.rect.width * scale,
                   .height = node.rect.height * scale};
  std::optional<Color> fill = node.style.background;
  const bool highlighted = node.state.focused || node.state.hovered;
  if (node.kind != NodeKind::kBox && highlighted && node.style.focus_background &&
      is_focusable(node)) {
    fill = node.style.focus_background;
  }
  if (fill) {
    draw_fill(rect, *fill);
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
  } else if (node.kind == NodeKind::kImage) {
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
