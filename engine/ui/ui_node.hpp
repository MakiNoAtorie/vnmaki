#pragma once

// The UI node tree (docs/SPEC.md section 15): what a screen is once built. A node has a kind, a
// style, an accessibility role, label and state, and the rectangle layout_tree() gave it. The
// tree is plain data; layout is in ui_layout.hpp and drawing in ui_renderer.hpp.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/runtime/script_runtime.hpp"

namespace vnmaki {

enum class NodeKind : std::uint8_t {
  kBox,
  kHbox,
  kVbox,
  kText,
  kImage,
  kButton,
  kToggle,
  kSlider,
  kBar,
  kInput
};

/// What a node is to assistive technology (docs/SPEC.md section 17).
enum class NodeRole : std::uint8_t {
  kGroup,
  kText,
  kImage,
  kButton,
  kToggle,
  kSlider,
  kProgress,
  kTextInput
};

/// Where children sit across the main axis' perpendicular, and along it.
enum class Align : std::uint8_t { kStretch, kStart, kCenter, kEnd };
enum class Justify : std::uint8_t { kStart, kCenter, kEnd, kSpaceBetween };

/// The state of a node that assistive technology and drawing read.
struct NodeState {
  bool focused = false;
  bool hovered = false;
  bool disabled = false;

  bool operator==(const NodeState&) const = default;
};

/// A toggle's caption sits this many text sizes right of its left padding edge; its mark (a
/// square one text size wide) is in the space.
inline constexpr float kToggleMarkEm = 1.5F;

/// The text size of a node that has none, in virtual pixels.
inline constexpr float kDefaultTextSize = 28;

/// The most characters an input takes when the script gives no `max_length`.
inline constexpr std::size_t kDefaultInputLength = 256;

/// All lengths are virtual pixels.
struct UiStyle {
  std::optional<float> width{};
  std::optional<float> height{};
  /// Share of the free space along the parent's main axis; 0 keeps the natural size.
  float grow = 0;
  float padding = 0;
  float margin = 0;
  /// Space between children.
  float gap = 0;
  Align align = Align::kStretch;
  Justify justify = Justify::kStart;
  /// Set: the node leaves the flow and sits at this offset from its parent's top-left corner.
  std::optional<PointF> position{};
  /// A fill behind the node.
  std::optional<Color> background{};
  /// The fill of a focused or hovered button instead of `background`.
  std::optional<Color> focus_background{};
  /// The fill of a hovered node instead of `background` (a focused node uses `focus_background`).
  std::optional<Color> hover_background{};
  /// The colour of text and button captions.
  Color color = kWhite;
  /// The colour of the caption of a focused or hovered node instead of `color`.
  std::optional<Color> focus_color{};
  std::optional<Color> hover_color{};
  /// A frame of `border_width` inside the node's rectangle. `border_color` is the colour of it
  /// (unset: `color`); a focused or hovered node can have another.
  float border_width = 0;
  std::optional<Color> border_color{};
  std::optional<Color> focus_border_color{};
  std::optional<Color> hover_border_color{};
  /// A family of the renderer's FontSet; empty is the base family.
  std::string font{};
  /// The filled part of a slider or bar, and the mark of a checked toggle.
  Color fill = kWhite;
  float text_size = kDefaultTextSize;
};

/// The datum a toggle, slider or bar shows and changes: `table` is "store", "persistent" or
/// "prefs", `key` a field of it (see UiHost::set_bindings).
struct NodeBinding {
  std::string table{};
  std::string key{};

  bool operator==(const NodeBinding&) const = default;
};

struct UiNode {
  NodeKind kind = NodeKind::kBox;
  NodeRole role = NodeRole::kGroup;
  /// What a screen reader says. Defaults to `text`.
  std::string label{};
  NodeState state{};
  UiStyle style{};
  /// The content: a text node's string, a button's caption, an image's name (`images/<name>.png`).
  std::string text{};
  /// The text of a text node or button before UiHost translated it (SPEC section 16); empty until
  /// then. UiHost translates again from this when the language changes.
  std::string source_text{};
  /// `text` has text tags (docs/TEXT_TAGS.md) that the renderer applies; a bad tag draws as plain
  /// text.
  bool markup = false;
  /// A text node that the typewriter reveals: the host sets `visible_glyphs` of it (the say box
  /// screen's line). The first such node of a tree is the one.
  bool reveal = false;
  /// A button hands this to its screen when activated; unset, activating does nothing.
  std::optional<ScreenValue> returns{};
  /// Root of a screen only: what Esc or the gamepad's east button hands to the screen (the same
  /// as a button's `returns`) while the screen has the keyboard; unset, they do nothing.
  std::optional<ScreenValue> cancel{};
  /// Root of a screen only: `show_screen` shows the screen as modal (it takes the keyboard and
  /// the clicks).
  bool modal = false;
  /// A text node shows only its first glyphs (a typewriter reveal); unset shows all.
  std::optional<std::size_t> visible_glyphs{};
  /// A toggle, slider or bar that follows a datum; UiHost refreshes `checked` and `value` from it
  /// and writes it when the player changes the node.
  std::optional<NodeBinding> bind{};
  /// A toggle is on.
  bool checked = false;
  /// An input shows `text` (the string it edits), or `placeholder` in a dim colour while `text`
  /// is empty. It takes at most `max_length` characters (Unicode code points).
  std::string placeholder{};
  std::size_t max_length = kDefaultInputLength;
  /// A slider's or bar's value, from `min` to `max`. A slider moves by `step` for a key press
  /// (0: a twentieth of the range).
  double value = 0;
  double min = 0;
  double max = 1;
  double step = 0;
  std::vector<UiNode> children{};
  /// Set by layout_tree(): the node's rectangle in virtual pixels, from the canvas' top-left.
  RectF rect{};
  /// The place of this node in the `children` of the script's node that holds it (a
  /// ScreenNodeDef). -1 for a root and for a node the engine added (the rows of a grid). The
  /// screen editor (engine/tools/screen_editor.hpp) maps a node back to its source with it.
  int source_index = -1;
};

/// A node of `kind` with the role that fits it.
[[nodiscard]] UiNode make_node(NodeKind kind);

/// Whether a button, toggle, slider or input can take the focus.
[[nodiscard]] bool is_focusable(const UiNode& node);

/// Multiplies the text size of `node` and all nodes under it by `factor` (the text scale
/// preference). A size stays at most kMaxFontPixelSize (docs/SPEC.md section 17).
void scale_text_sizes(UiNode& node, float factor);

/// What a node draws and measures as text: its `text`, or the placeholder of an empty input.
[[nodiscard]] const std::string& display_text(const UiNode& node);

}  // namespace vnmaki
