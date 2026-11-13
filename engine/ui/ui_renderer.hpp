#pragma once

// Draws a laid-out UI tree through IRenderer (docs/SPEC.md section 15): fills, button states,
// text and images. Use it from the thread that owns the renderer and the font.

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "engine/platform/geometry.hpp"
#include "engine/render/image.hpp"
#include "engine/render/renderer.hpp"
#include "engine/text/text.hpp"
#include "engine/ui/ui_layout.hpp"
#include "engine/ui/ui_node.hpp"

namespace vnmaki {

class Vfs;

/// An image node named "gen:<anything>" is not a file: set_generated_images() makes it.
inline constexpr std::string_view kGeneratedImagePrefix = "gen:";

class UiRenderer {
 public:
  /// `renderer` and the fonts must outlive this object. `vfs` (may be null, then image nodes draw
  /// nothing) reads `images/<name>.png`, `.jpg` or `.webp` for image nodes; it must outlive this too.
  UiRenderer(IRenderer& renderer, FontSet fonts, const Vfs* vfs = nullptr);
  /// One font, no fallbacks.
  UiRenderer(IRenderer& renderer, const Font& font, const Vfs* vfs = nullptr);
  UiRenderer(const UiRenderer&) = delete;
  UiRenderer& operator=(const UiRenderer&) = delete;
  UiRenderer(UiRenderer&&) = delete;
  UiRenderer& operator=(UiRenderer&&) = delete;
  ~UiRenderer();

  /// Gives the pixels of an image node whose name starts with kGeneratedImagePrefix, by that
  /// name (save thumbnails). Empty: such nodes draw nothing. The texture is kept under the name,
  /// so a changed image needs a new name.
  void set_generated_images(std::function<std::optional<Image>(const std::string&)> source) {
    generated_ = std::move(source);
  }

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
  // A GlyphAtlas drops its glyphs when the layout's base size changes, so each text size has one.
  struct SizeAtlas {
    GlyphAtlas atlas{};
    std::optional<TextureId> texture{};
  };

  [[nodiscard]] const TextLayout& layout(const UiNode& node, float wrap_width);
  [[nodiscard]] std::optional<TextureId> image_texture(const std::string& name);
  void draw_node(const UiNode& node, float scale, PointF origin);
  void draw_fill(RectF rect, Color color);
  void draw_text(const UiNode& node, float scale, PointF origin);
  void draw_toggle_mark(const UiNode& node, float scale, PointF origin);

  IRenderer* renderer_;
  FontSet fonts_;
  const Vfs* vfs_;
  std::function<std::optional<Image>(const std::string&)> generated_;
  TextureId white_;
  std::map<std::tuple<std::string, bool, float, float>, TextLayout> layouts_;
  std::map<float, SizeAtlas> atlases_;
  std::map<std::string, std::optional<TextureId>> images_;
};

}  // namespace vnmaki
