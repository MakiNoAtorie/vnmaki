#pragma once

// The skip and auto indicator: a small label at the top-right corner of the canvas that shows
// while skip or auto-advance is on. It is an ordinary screen of the UiHost, so UiRenderer draws
// it with the others.

#include <string>
#include <string_view>

#include "engine/platform/geometry.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/ui/ui_node.hpp"

namespace vnmaki {

/// The name of the indicator screen.
inline constexpr std::string_view kPlaybackScreenName = "vnmaki:playback";

/// "SKIP", "SKIP ALL", "AUTO" or "AUTO SKIP"; empty when both are off.
[[nodiscard]] std::string playback_indicator_text(SkipMode skip, bool automatic);

/// The indicator screen for a canvas of `canvas` pixels, showing `text`.
[[nodiscard]] UiNode make_playback_screen(Size canvas, std::string_view text);

class PlaybackIndicator {
 public:
  /// `host` must outlive this.
  explicit PlaybackIndicator(UiHost& host) : host_(&host) {}

  /// Shows the screen with the text for `skip` and `automatic`, or hides it if both are off.
  /// Call it every frame; it changes the host only when the text changes.
  void sync(SkipMode skip, bool automatic);

 private:
  UiHost* host_;
  std::string shown_{};
};

}  // namespace vnmaki
