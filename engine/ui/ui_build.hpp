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
///   text        markup (boolean: the text has text tags), reveal (boolean: the typewriter
///               shows this node's glyphs one by one; see SayBoxView)
///   button      returns (what the screen returns when the button is activated; unset, the button
///               does nothing), focus_bg (colour), disabled (boolean)
///   toggle      a caption like a button, and bind_table and bind_key (what the script's
///               bind(table, key) gives), checked (boolean; the start state of an unbound
///               toggle), focus_bg, disabled
///   slider      bind_table and bind_key, min and max (numbers, min below max; default 0 and
///               1), value (the start value of an unbound slider), step (number above 0), focus_bg, disabled
///   bar         bind_table, bind_key, min, max and value: a slider the player cannot change
///   grid        columns (a whole number from 1 to 64, required): the children go left to right,
///               top to bottom; the cells keep their own sizes
///   image       width and height are required
///   any root    modal (boolean: `show_screen` shows it modal), cancel (what Esc or the gamepad's
///               east button return from it)
///
/// A colour is "#rrggbb" or "#rrggbbaa". Throws UiBuildError, naming the node and the option.
[[nodiscard]] UiNode build_ui_tree(const ScreenNodeDef& def);

/// The choice menu of a `menu { ... }`: the options as buttons in a column in the middle of the
/// canvas. Button i returns the integer i + 1, the number `menu` gives back.
[[nodiscard]] UiNode make_menu_screen(const std::vector<std::string>& options, Size canvas);

}  // namespace vnmaki
