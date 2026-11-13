#pragma once

// Captions for sounds and music (docs/SPEC.md section 17). `sound`, `music` and `ambient` take
// `caption = "..."`; Audio::take_captions() hands the captions over, and the presenter shows them
// on the Lua screen `captions` (not modal) for the length that Audio gave, when the player turns
// `prefs.captions` on.

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/audio/audio.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/ui/ui_host.hpp"

namespace vnmaki {

/// The name of the captions screen.
inline constexpr std::string_view kCaptionsScreenName = "captions";

/// The most captions that show at once; the oldest go first.
inline constexpr std::size_t kMaxCaptions = 4;

class CaptionPresenter {
 public:
  /// `runtime` and `host` must outlive this. `on_warning` gets the reason the screen fails.
  CaptionPresenter(ScriptRuntime& runtime, UiHost& host,
                   std::function<void(std::string_view)> on_warning = {});

  /// Call it once a frame: `fresh` are the captions made since the last call
  /// (Audio::take_captions()), `seconds` the time since the last call. With `enabled` false, the
  /// captions are dropped. The screen changes only when the list of captions does.
  void update(std::vector<AudioCaption> fresh, double seconds, bool enabled);

  /// The captions that show now, oldest first.
  [[nodiscard]] std::vector<std::string> shown() const;

 private:
  void show_screen();

  ScriptRuntime* runtime_;
  UiHost* host_;
  std::function<void(std::string_view)> on_warning_;
  std::vector<AudioCaption> active_{};  ///< `seconds` is the time left.
  std::vector<std::string> on_screen_{};
};

}  // namespace vnmaki
