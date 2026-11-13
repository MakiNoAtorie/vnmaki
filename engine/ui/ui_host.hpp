#pragma once

// The screen host (docs/SPEC.md section 15): the stack of shown screens, their layout and the
// input that moves the focus and activates buttons. It needs no window or GPU.
//
// A screen is a UI tree with a name. Showing a screen with a name that is already shown replaces
// it. The last screen shown is on top. A modal screen (a menu, a `call_screen`) takes the
// keyboard and gamepad and swallows pointer clicks; other screens take the pointer only, so a
// quick menu cannot steal the keys that advance the dialogue.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/platform/geometry.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/viewport.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/ui/ui_layout.hpp"
#include "engine/ui/ui_node.hpp"

namespace vnmaki {

/// A button with a `returns` value was activated.
struct ScreenActivation {
  std::string screen{};
  ScreenValue value{};
};

/// What the host did with an input event.
struct UiEventResult {
  /// The event was for the UI: the caller must not use it for anything else.
  bool consumed = false;
  std::optional<ScreenActivation> activation{};
};

class UiHost final : public ScreenCommands {
 public:
  /// `canvas` is the virtual resolution. Until set_text_measure() is called, text is sized by
  /// estimate_text_size().
  explicit UiHost(Size canvas);

  void set_text_measure(TextMeasure measure);
  [[nodiscard]] Size canvas() const { return canvas_; }

  /// ScreenCommands, for the scripts' `show_screen` and `hide_screen`: builds the tree with
  /// build_ui_tree() and shows it, not modal. Returns why it cannot, or an empty string.
  [[nodiscard]] std::string check_screen(std::string_view name,
                                         const ScreenNodeDef& screen) override;
  [[nodiscard]] std::string show_screen(std::string_view name, const ScreenNodeDef& screen) override;
  void hide_screen(std::string_view name) override;

  /// Shows `root` as screen `name` on top. A modal screen focuses its first button.
  void show(std::string name, UiNode root, bool modal);
  /// Returns whether a screen of that name was shown.
  bool hide(std::string_view name);
  [[nodiscard]] bool is_shown(std::string_view name) const;
  [[nodiscard]] bool has_modal() const;

  /// The tree of screen `name`, laid out; null if it is not shown.
  [[nodiscard]] UiNode* find(std::string_view name);
  /// The trees of all shown screens, laid out, bottom first.
  [[nodiscard]] std::vector<const UiNode*> roots();

  /// Handles one input. Pointer positions are in window pixels; `viewport` maps them to the
  /// canvas. Moving the pointer hovers (and focuses) the button under it. A left click on a
  /// button activates it. On the top modal screen: Tab and Shift+Tab, the arrow keys and the
  /// d-pad move the focus; Enter, Space and the south gamepad button activate the focused button.
  [[nodiscard]] UiEventResult handle_event(const InputEvent& event, const Viewport& viewport);

 private:
  struct Screen {
    std::string name{};
    UiNode root{};
    bool modal = false;
  };

  void ensure_layout();
  [[nodiscard]] Screen* top_modal();
  [[nodiscard]] UiEventResult pointer_event(PointF point, bool click);
  [[nodiscard]] UiEventResult activate_focused();

  Size canvas_;
  TextMeasure measure_;
  std::vector<Screen> screens_;
  bool dirty_ = false;
};

}  // namespace vnmaki
