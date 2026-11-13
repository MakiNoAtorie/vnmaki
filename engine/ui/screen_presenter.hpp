#pragma once

// Connects a started ScriptRuntime to a UiHost for the requests that need a screen: `menu { ... }`
// and `call_screen(...)`. say and narrate lines are the DialoguePresenter's business.

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "engine/platform/input.hpp"
#include "engine/platform/viewport.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/ui/ui_host.hpp"

namespace vnmaki {

/// The name of the modal screen that shows a menu. A `call_screen` screen has its own name.
inline constexpr std::string_view kMenuScreenName = "vnmaki:menu";

class ScreenPresenter {
 public:
  /// `runtime` and `host` must outlive the presenter. Calls sync().
  ScreenPresenter(ScriptRuntime& runtime, UiHost& host);

  /// Shows the modal screen of the request the script waits at, if it is a menu or a
  /// `call_screen`, and hides it once the script moves on. Call it after anything that can move
  /// the story (it is called after every answer this class gives).
  void sync();

  /// True while the script waits at a menu or a `call_screen`.
  [[nodiscard]] bool active() const { return shown_.has_value(); }

  /// Passes `event` to the host. A button of the shown screen that is activated answers the
  /// script: a menu button chooses its option, and `call_screen` returns the button's `returns`
  /// value. Returns whether the event was for the UI.
  bool handle_event(const InputEvent& event, const Viewport& viewport);

 private:
  void answer(const ScreenValue& value);

  ScriptRuntime* runtime_;
  UiHost* host_;
  // The screen shown for the current request, and where the script waited when it was shown.
  std::optional<std::pair<std::string, ScriptPosition>> shown_;
};

}  // namespace vnmaki
