#pragma once

// The screen host (docs/SPEC.md section 15): the stack of shown screens, their layout and the
// input that moves the focus and activates buttons. It needs no window or GPU.
//
// A screen is a UI tree with a name. Showing a screen with a name that is already shown replaces
// it. The last screen shown is on top. A modal screen (a menu, a `call_screen`, a screen whose
// root has `modal = true`) takes the keyboard and gamepad and swallows pointer clicks; other
// screens take the pointer only, so a quick menu cannot steal the keys that advance the dialogue.
// The player can still reach a non-modal screen with the keyboard: Tab or the gamepad's west
// button gives it the keys until Esc or the east button takes them back. A click on an input gives
// its screen the keys too.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/platform/geometry.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/platform.hpp"
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
  /// Multiplies the text of every screen, shown now or later, by `scale` (the `text_scale`
  /// preference; 1 is the size the screen says). The layout is done again. A tree given to show()
  /// has the sizes the screen says; the host scales it.
  void set_text_scale(float scale);
  [[nodiscard]] float text_scale() const { return text_scale_; }
  /// Lays the screens out again at the next use: call it when the fonts that the measure uses
  /// changed.
  void relayout() { dirty_ = true; }
  /// Where toggles, sliders and bars with a `bind` read and write their data (ScriptRuntime::
  /// binding() and set_binding()). Without it they keep their own value. `runtime` must outlive
  /// the host; null unsets it.
  void set_bindings(ScriptRuntime* runtime) { bindings_ = runtime; }
  [[nodiscard]] Size canvas() const { return canvas_; }
  /// The platform whose text input the host turns on while an input has the keys, and off
  /// otherwise (docs/SPEC.md section 17). `platform` must outlive the host; null unsets it.
  void set_platform(Platform* platform);
  /// Whether an input has the keys now: the keyboard screen's focused node is an enabled input.
  /// Then typed text, Backspace and every other key go to the input, and the story window must
  /// not use them (the keys A, S and L).
  [[nodiscard]] bool wants_text_input();

  /// ScreenCommands, for the scripts' `show_screen` and `hide_screen`: builds the tree with
  /// build_ui_tree() and shows it, not modal. Returns why it cannot, or an empty string.
  [[nodiscard]] std::string check_screen(std::string_view name,
                                         const ScreenNodeDef& screen) override;
  [[nodiscard]] std::string show_screen(std::string_view name,
                                        const ScreenNodeDef& screen) override;
  void hide_screen(std::string_view name) override;

  /// Shows `root` as screen `name` on top. A modal screen focuses its first button.
  void show(std::string name, UiNode root, bool modal);
  /// Returns whether a screen of that name was shown.
  bool hide(std::string_view name);
  [[nodiscard]] bool is_shown(std::string_view name) const;
  [[nodiscard]] bool has_modal() const;
  /// Whether screen `name` is the topmost modal screen.
  [[nodiscard]] bool is_top_modal(std::string_view name) const;

  /// The names of the shown screens, bottom first.
  [[nodiscard]] std::vector<std::string> screen_names() const;

  /// The tree of screen `name`, laid out; null if it is not shown.
  [[nodiscard]] UiNode* find(std::string_view name);
  /// The trees of all shown screens, laid out, bottom first.
  [[nodiscard]] std::vector<const UiNode*> roots();

  /// Handles one input. Pointer positions are in window pixels; `viewport` maps them to the
  /// canvas. Moving the pointer hovers (and focuses) the button under it. A left click on a
  /// button activates it, flips a toggle, and sets a slider (dragging moves it). On the screen that
  /// has the keyboard (the top modal screen, or the one Tab gave it to): Tab and Shift+Tab, the
  /// arrow keys and the d-pad move the focus; Enter, Space and the south gamepad button activate
  /// the focused button or toggle; Left and Right move the focused slider. Esc and the east
  /// button give the root's `cancel` value, or take the keys back from a non-modal screen.
  /// While any modal screen is shown, every player input is consumed (also a wheel turn, Page Up
  /// and the shoulder buttons that the story window rolls back on). Window and gamepad connection
  /// events and a quit request are not.
  /// While an input has the keys, a TextInputEvent adds its text at the end (up to `max_length`
  /// characters) and Backspace removes the last character; Enter gives the input's `returns`;
  /// Tab, the arrow keys and Esc work as always; other keys are consumed and do nothing. Moving
  /// the pointer does not take the focus from an input; a click does.
  [[nodiscard]] UiEventResult handle_event(const InputEvent& event, const Viewport& viewport);

 private:
  struct Screen {
    std::string name{};
    UiNode root{};
    bool modal = false;
  };

  void ensure_layout();
  void refresh_bindings();
  [[nodiscard]] UiEventResult dispatch_event(const InputEvent& event, const Viewport& viewport);
  [[nodiscard]] UiNode* focused_input();
  [[nodiscard]] UiEventResult text_event(const TextInputEvent& typed);
  void sync_text_input();
  void write_value(UiNode& node, ScreenValue value);
  void set_slider(UiNode& node, double value);
  void drag_slider(PointF point);
  [[nodiscard]] Screen* top_modal();
  [[nodiscard]] Screen* keyboard_screen();
  [[nodiscard]] UiEventResult key_event(const InputEvent& event);
  [[nodiscard]] UiEventResult pointer_event(PointF point, bool click);
  [[nodiscard]] UiEventResult activate_focused(Screen& screen);

  Size canvas_;
  float text_scale_ = 1;
  ScriptRuntime* bindings_ = nullptr;
  Platform* platform_ = nullptr;
  bool text_input_on_ = false;
  // The non-modal screen the player gave the keyboard to (Tab); empty for none.
  std::string captured_;
  // A slider the pointer drags: its screen and its place among the screen's focusable nodes.
  std::optional<std::pair<std::string, std::size_t>> drag_;
  TextMeasure measure_;
  std::vector<Screen> screens_;
  bool dirty_ = false;
};

}  // namespace vnmaki
