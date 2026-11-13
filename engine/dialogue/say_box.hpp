#pragma once

// The say box (docs/SPEC.md section 7): a panel at a fixed place in virtual resolution with the
// speaker's name and the typed-out line. It is a UI screen (engine/ui/): SayBoxStyle describes its
// look until a Lua screen replaces it.

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

#include "engine/dialogue/dialogue_presenter.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/text/text.hpp"
#include "engine/ui/ui_node.hpp"
#include "engine/ui/ui_renderer.hpp"

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

/// The say box as a screen: a canvas-sized box holding the panel, the speaker's name and the line
/// (its first `visible_glyphs` glyphs; unset shows all). The nodes sit where `style` says; layout
/// them with layout_tree() and draw them with UiRenderer. The line has text tags.
[[nodiscard]] UiNode make_say_screen(const SayBoxStyle& style, const DialogueLine& line,
                                     std::optional<std::size_t> visible_glyphs = std::nullopt);

/// Draws the say box screen. The line may have text tags (docs/TEXT_TAGS.md); the speaker's name
/// is plain text. Owns the textures it makes; `renderer` and the fonts must outlive it. Use it
/// from the thread that owns the renderer and the fonts.
class SayBoxView {
 public:
  /// One font, no fallbacks.
  SayBoxView(IRenderer& renderer, const Font& font, SayBoxStyle style);
  /// `fonts` is the base family, the `font` tag families and the fallback chain.
  SayBoxView(IRenderer& renderer, FontSet fonts, SayBoxStyle style);
  SayBoxView(const SayBoxView&) = delete;
  SayBoxView& operator=(const SayBoxView&) = delete;
  SayBoxView(SayBoxView&&) = delete;
  SayBoxView& operator=(SayBoxView&&) = delete;
  ~SayBoxView() = default;

  /// Builds the screen of a line from a script (the Lua screen `say`) in place of `style`'s box.
  /// `source` returns the screen tree, or nothing when the script cannot build it (the view then
  /// uses `style`). The tree has a text node with `reveal` (UiNode::reveal): the line. `canvas`
  /// is the virtual resolution the tree lays out in. The tree is built once for each line.
  using ScreenSource = std::function<std::optional<UiNode>(const DialogueLine&)>;
  void set_screen_source(ScreenSource source, Size canvas);

  /// Lays `line` out and returns its glyph count (what DialoguePresenter needs).
  [[nodiscard]] std::size_t glyph_count(const DialogueLine& line);

  /// How many glyphs of `line` show `elapsed_seconds` after it started, with the `speed` and
  /// `pause` tags of its text (see revealed_glyphs).
  [[nodiscard]] std::size_t revealed(const DialogueLine& line, double characters_per_second,
                                     double elapsed_seconds);

  /// The id of the `link` tag of `line` at `point` (virtual pixels), or an empty string.
  [[nodiscard]] std::string link_at(const DialogueLine& line, PointF point);

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
  [[nodiscard]] UiNode* scripted_screen(const DialogueLine& line);
  [[nodiscard]] UiNode laid_out_screen(const DialogueLine& line,
                                       std::optional<std::size_t> visible_glyphs);

  ScreenSource source_;
  Size canvas_{};
  // The laid-out tree of `built_for_` (empty if the source gave none) and where its line starts.
  std::optional<DialogueLine> built_for_;
  std::optional<UiNode> built_;
  FontSet fonts_;
  SayBoxStyle style_;
  UiRenderer ui_;
  std::string laid_out_text_;
  bool has_layout_ = false;
  TextLayout text_layout_;
};

}  // namespace vnmaki
