#pragma once

// Focus and hit testing on a laid-out UI tree (docs/SPEC.md section 15). The focus is the
// `state.focused` flag of one focusable node (an enabled button). Tab order is document order.

#include <cstdint>
#include <vector>

#include "engine/platform/geometry.hpp"
#include "engine/ui/ui_node.hpp"

namespace vnmaki {

enum class FocusMove : std::uint8_t { kNext, kPrevious, kUp, kDown, kLeft, kRight };

/// The focusable nodes of the tree in document order.
[[nodiscard]] std::vector<UiNode*> focusable_nodes(UiNode& root);

/// The node that has the focus; null if none.
[[nodiscard]] UiNode* focused_node(UiNode& root);

/// Gives the focus to `target` (null clears it). Returns whether the focused node changed.
bool set_focus(UiNode& root, UiNode* target);

/// Moves the focus. kNext and kPrevious follow document order and wrap around. The four
/// directions pick the nearest focusable node whose centre lies that way from the focused one
/// (an offset across the direction counts double) and stay put if there is none. With nothing
/// focused, any move focuses the first node (the last for kPrevious). Returns whether the focus
/// changed.
bool move_focus(UiNode& root, FocusMove move);

/// Pointer hover at `point` (virtual pixels): marks the topmost focusable node under it as
/// hovered, clears the mark on the others and moves the focus there. Returns whether the focus
/// changed.
bool hover_at(UiNode& root, PointF point);

/// Clears the hovered mark of every node of the tree.
void clear_hover(UiNode& root);

/// The topmost focusable node under `point`; null if none.
[[nodiscard]] UiNode* focusable_at(UiNode& root, PointF point);

}  // namespace vnmaki
