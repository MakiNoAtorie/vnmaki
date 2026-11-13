#pragma once

// Flexbox layout of a UI tree with Yoga (docs/SPEC.md section 15). Yoga is private to the engine:
// this header names none of its types.

#include <functional>

#include "engine/l10n/text_direction.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/ui/ui_node.hpp"

namespace vnmaki {

struct MeasuredSize {
  float width = 0;
  float height = 0;
};

/// The size of the content of a text node or a button caption when it wraps at `max_width`
/// (infinite for no wrap), without the node's padding.
using TextMeasure = std::function<MeasuredSize(const UiNode& node, float max_width)>;

/// A guess for tests and headless runs that have no font: half an em for each character, one em
/// high, no wrapping.
// ponytail: no wrapping; a headless run with long wrapped lines needs the real measure.
[[nodiscard]] MeasuredSize estimate_text_size(const UiNode& node, float max_width);

/// Sets `rect` of every node of the tree under `root`, in virtual pixels from the canvas'
/// top-left corner. `root` fills the canvas. `box` and `vbox` stack their children top to bottom,
/// `hbox` left to right; a node with a position leaves the flow. Text nodes and buttons without
/// children are sized by `measure`. In a right-to-left language (`direction`) an `hbox` puts its
/// first child at the right edge, so the children run right to left.
void layout_tree(UiNode& root, Size canvas, const TextMeasure& measure,
                 TextDirection direction = TextDirection::kLtr);

}  // namespace vnmaki
