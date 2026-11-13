#pragma once

// The say box (docs/SPEC.md section 7): a panel at a fixed place in virtual resolution with the
// speaker's name and the typed-out line. Its look is hard-coded here until screens (ticket 10)
// describe it.

#include <cstddef>
#include <optional>
#include <string>

#include "engine/dialogue/dialogue_presenter.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/text/text.hpp"

namespace vnmaki {

/// Where and how the say box is drawn. All lengths are virtual pixels.
struct SayBoxStyle {
  RectF box{};
  Color box_color{};
  /// From the top-left corner of the box to the top-left corner of the name and of the line.
  PointF name_offset{};
  PointF text_offset{};
  float name_size = 0;  ///< Pixels per em.
  float text_size = 0;
  /// The line wraps at this width.
  float wrap_width = 0;
  Color text_color = kWhite;

  /// The look for a virtual resolution: a box across the bottom with margins on a 16:9 canvas.
  [[nodiscard]] static SayBoxStyle for_resolution(Size virtual_size);
};

/// Draws the say box with one font. Owns the textures it makes; `renderer` and `font` must outlive
/// it. Use it from the thread that owns the renderer and the font.
class SayBoxView {
 public:
  SayBoxView(IRenderer& renderer, const Font& font, SayBoxStyle style);
  SayBoxView(const SayBoxView&) = delete;
  SayBoxView& operator=(const SayBoxView&) = delete;
  SayBoxView(SayBoxView&&) = delete;
  SayBoxView& operator=(SayBoxView&&) = delete;
  ~SayBoxView();

  /// Lays `line` out and returns its glyph count (what DialoguePresenter needs).
  [[nodiscard]] std::size_t glyph_count(const DialogueLine& line);

  /// Rasterizes the glyphs of `line` at `scale` pixels per virtual pixel and uploads them. draw()
  /// does this too, but uploading inside a frame is slow: call prepare() before begin_frame().
  /// Throws RenderError and FontError.
  void prepare(const DialogueLine& line, float scale);

  /// Draws the box and the first `visible_glyphs` glyphs of the line into the renderer's current
  /// pass. The pass target has `scale` pixels for each virtual pixel (the viewport scale), and
  /// the text is rasterized at that scale so it stays sharp. `origin` is where the virtual
  /// canvas starts in the target (the viewport's top-left corner), in whole pixels.
  void draw(const DialogueLine& line, std::size_t visible_glyphs, float scale, PointF origin = {});

 private:
  void layout(const DialogueLine& line);
  void upload(const GlyphAtlas& atlas, std::optional<TextureId>& texture);
  void prepare_text(const TextLayout& text, GlyphAtlas& atlas, std::optional<TextureId>& texture,
                    float scale);
  void draw_text(const TextLayout& text, const GlyphAtlas& atlas,
                 const std::optional<TextureId>& texture, PointF origin, std::size_t visible_glyphs,
                 Color tint);

  IRenderer* renderer_;
  const Font* font_;
  SayBoxStyle style_;
  TextureId white_;
  GlyphAtlas name_atlas_;
  GlyphAtlas text_atlas_;
  std::optional<TextureId> name_texture_;
  std::optional<TextureId> text_texture_;
  std::string laid_out_speaker_;
  std::string laid_out_text_;
  bool has_layout_ = false;
  TextLayout name_layout_;
  TextLayout text_layout_;
};

}  // namespace vnmaki
