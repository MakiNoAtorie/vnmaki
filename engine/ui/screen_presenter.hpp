#pragma once

// Connects a started ScriptRuntime to a UiHost for the requests that need a screen: `menu { ... }`
// and `call_screen(...)`. say and narrate lines are the DialoguePresenter's business.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/platform/input.hpp"
#include "engine/platform/viewport.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/ui/ui_host.hpp"

namespace vnmaki {

/// The name of the modal screen that shows a menu: the Lua screen `choice` (the options as its
/// args), or make_menu_screen() when no such screen is defined. A `call_screen` screen has its
/// own name.
inline constexpr std::string_view kMenuScreenName = "vnmaki:menu";

class ScreenPresenter {
 public:
  /// `runtime` and `host` must outlive the presenter. Calls sync().
  ScreenPresenter(ScriptRuntime& runtime, UiHost& host);

  /// Shows the modal screen of the request the script waits at, if it is a menu or a
  /// `call_screen`, and hides it once the script moves on. Call it after anything that can move
  /// the story (it is called after every answer this class gives).
  void sync();

  /// Shows the options of a menu in the language of `translator` (the scope of the key is the
  /// running label). A change of its revision shows the menu again. A `call_screen` screen is
  /// translated by the UiHost. `translator` must outlive the presenter; null shows the source text.
  void set_translator(const Translator* translator) { translator_ = translator; }

  /// True while the script waits at a menu or a `call_screen`.
  [[nodiscard]] bool active() const { return shown_.has_value(); }

  /// Gives an activation that no script waits for, or that returns an engine action ("@..."),
  /// to `handler` first; if it returns true, the activation is done. See ScreenActions::handle.
  void set_action_handler(std::function<bool(const ScreenActivation&)> handler) {
    actions_ = std::move(handler);
  }

  /// Passes `event` to the host. A button of the shown screen that is activated answers the
  /// script: a menu button chooses its option (the screen returns its number, from 1), and
  /// `call_screen` returns the button's `returns` value. Returns whether the event was for the UI.
  bool handle_event(const InputEvent& event, const Viewport& viewport);

 private:
  void answer(const ScreenValue& value);
  [[nodiscard]] UiNode menu_tree(const MenuRequest& menu);

  ScriptRuntime* runtime_;
  UiHost* host_;
  const Translator* translator_ = nullptr;
  std::function<bool(const ScreenActivation&)> actions_;
  // The screen shown for a request, and where the script waited when it was shown.
  struct Shown {
    std::string name;
    ScriptPosition position;
    BlockingRequest request;
    std::uint64_t revision;  // of the translator
  };
  std::optional<Shown> shown_;
};

}  // namespace vnmaki
