#pragma once

// Builds UI trees: from the nodes a script wrote (ScreenNodeDef) and for the screens the engine
// makes itself (the choice menu).

#include <stdexcept>
#include <string>
#include <vector>

#include "engine/platform/geometry.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/ui/ui_node.hpp"

namespace vnmaki {

/// A screen definition is wrong: an unknown node kind or option, or an option of the wrong type.
class UiBuildError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Turns a script's node tree into a UI tree. The options are:
///
///   all nodes   width, height, grow, padding, margin, gap (numbers, virtual pixels, not
///               negative); align ("stretch", "start", "center", "end"); justify ("start",
///               "center", "end", "space_between"); x and y (numbers: the node leaves the flow
///               and sits at that offset in its parent); bg (colour); color (colour of text);
///               size (text size); label (what a screen reader says, instead of the text)
///   button      returns (what the screen returns when the button is activated; unset, the button
///               does nothing), focus_bg (colour), disabled (boolean)
///   image       width and height are required
///
/// A colour is "#rrggbb" or "#rrggbbaa". Throws UiBuildError, naming the node and the option.
[[nodiscard]] UiNode build_ui_tree(const ScreenNodeDef& def);

/// The choice menu of a `menu { ... }`: the options as buttons in a column in the middle of the
/// canvas. Button i returns the integer i (from 0).
[[nodiscard]] UiNode make_menu_screen(const std::vector<std::string>& options, Size canvas);

}  // namespace vnmaki
