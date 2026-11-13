#pragma once

// Draws a laid-out UI tree through IRenderer (docs/SPEC.md section 15): fills, button states,
// text and images. Use it from the thread that owns the renderer and the font.

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <tuple>

#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/text/text.hpp"
#include "engine/ui/ui_layout.hpp"
#include "engine/ui/ui_node.hpp"

namespace vnmaki {

class Vfs;

class UiRenderer {
 public:
  /// `renderer` and `font` must outlive this object. `vfs` (may be null, then image nodes draw
  /// nothing) reads `images/<name>.png` or `.jpg` for image nodes; it must outlive this too.
  UiRenderer(IRenderer& renderer, const Font& font, const Vfs* vfs = nullptr);
  UiRenderer(const UiRenderer&) = delete;
  UiRenderer& operator=(const UiRenderer&) = delete;
  UiRenderer(UiRenderer&&) = delete;
  UiRenderer& operator=(UiRenderer&&) = delete;
  ~UiRenderer();

  /// The measure that sizes text with the font. Valid while this object lives.
  [[nodiscard]] TextMeasure text_measure();

  /// Rasterizes the glyphs of the tree at `scale` pixels per virtual pixel and uploads them.
  /// draw() does this too, but uploading inside a frame is slow: call prepare() before
  /// begin_frame(). Throws RenderError and FontError.
  void prepare(const UiNode& root, float scale);

  /// Draws the tree into the renderer's current pass. The target has `scale` pixels for each
  /// virtual pixel, and `origin` is where the canvas starts in it (the viewport's top-left).
  void draw(const UiNode& root, float scale, PointF origin = {});

 private:
  struct SizeAtlas {
    explicit SizeAtlas(const Font& font) : atlas(font) {}
    GlyphAtlas atlas;
    std::optional<TextureId> texture{};
  };

  [[nodiscard]] const TextLayout& layout(const UiNode& node, float wrap_width);
  [[nodiscard]] SizeAtlas& atlas_for(float text_size);
  [[nodiscard]] std::optional<TextureId> image_texture(const std::string& name);
  void draw_node(const UiNode& node, float scale, PointF origin);
  void draw_fill(RectF rect, Color color);
  void draw_text(const UiNode& node, float scale, PointF origin);

  IRenderer* renderer_;
  const Font* font_;
  const Vfs* vfs_;
  TextureId white_;
  std::map<std::tuple<std::string, float, float>, TextLayout> layouts_;
  std::map<float, SizeAtlas> atlases_;
  std::map<std::string, std::optional<TextureId>> images_;
};

}  // namespace vnmaki
