#pragma once

// The engine's screen actions (docs/SPEC.md section 15). A button of a screen that returns a string
// starting with "@" asks the engine to do something; a script's own `call_screen` values never
// start with "@". ScreenActions carries the actions out:
//
//   @open:<screen>   show Lua screen <screen> (modal). save and load get the slot list as args
//   @close           hide the screen of the button, and the ones it opened
//   @view:<image>    show the Lua screen gallery_view for an unlocked gallery image (modal)
//   @save:<slot>     save to the slot; a slot that is in use asks "confirm" first
//   @load:<slot>     load the slot and close the screens that were opened
//   @auto            turn auto-advance on or off
//   @skip            turn skip on (in the mode of the `skip_unseen` preference) or off
//   @backlog         open the backlog
//   @language:<code> set the preference `language` and change the language at once
//   @quit            ask "confirm", then call the quit hook
//
// `@save!<slot>` and `@quit!` are what the confirm screen's Yes button returns.

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/render/image.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/saves/saves.hpp"
#include "engine/ui/ui_host.hpp"

namespace vnmaki {

class BacklogScreen;
class Playback;

/// How many numbered slots the save and load screens list.
inline constexpr int kScreenSlotCount = 12;

struct ScreenActionTargets {
  /// For @auto and @skip. Null: they do nothing.
  Playback* playback = nullptr;
  /// For @backlog. Null: it does nothing.
  BacklogScreen* backlog = nullptr;
  /// Called when the player confirms @quit. Without it, @quit does nothing.
  std::function<void()> quit{};
  /// Called with the code for @language:<code>, after the preference is written. It changes the
  /// language (the story window does). Without it, @language only writes the preference.
  std::function<void(const std::string&)> language{};
  /// Receives the reasons an action failed (a missing screen, a save that cannot be written).
  std::function<void(std::string_view)> on_warning{};
};

class ScreenActions {
 public:
  /// All of these must outlive this object.
  ScreenActions(ScriptRuntime& runtime, UiHost& host, Saves& saves, ScreenActionTargets targets);

  /// Whether `value` is an engine action ("@..."). It does not need a ScreenActions.
  [[nodiscard]] static bool is_action(const ScreenValue& value);

  /// Carries out the action the activation returns. Returns false, doing nothing, when the value
  /// is not an action.
  bool handle(const ScreenActivation& activation);

  /// Shows Lua screen `name` with `args`. A modal screen is remembered, so @close and a load hide
  /// it; a screen that is not modal (the quick menu) stays until a script hides it. Returns why it
  /// cannot, or an empty string.
  std::string open(std::string_view name, const ScreenNodeDef& args = {}, bool modal = true);

  /// The thumbnail of a slot entry, by the image name its screen got (UiRenderer::
  /// set_generated_images).
  [[nodiscard]] std::optional<Image> thumbnail(const std::string& image_name) const;

 private:
  void warn(std::string_view message) const;
  [[nodiscard]] ScreenNodeDef slot_args(bool saving);
  void ask(std::string_view message, std::string yes, std::string no);
  void close_all();
  void do_save(const std::string& slot);

  ScriptRuntime* runtime_;
  UiHost* host_;
  Saves* saves_;
  ScreenActionTargets targets_;
  // The screens `open` showed, bottom first.
  std::vector<std::string> opened_;
  // The args each of them was opened with, so that @language can build it again.
  std::map<std::string, ScreenNodeDef> opened_args_;
  std::map<std::string, Thumbnail> thumbnails_;
};

}  // namespace vnmaki
