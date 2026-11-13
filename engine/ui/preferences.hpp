#pragma once

// The player's preferences (docs/SPEC.md sections 15 and 17). The values live in `prefs`, a view of
// `persistent.prefs` with defaults (script_runtime.hpp), so the preferences screen binds to them
// and they are saved with the persistent file. PreferenceApplier reads them and applies each
// change: text speed to the reveal, auto delay to Playback, volumes to Audio, fullscreen and window
// opacity to the Platform.

#include <optional>

#include "engine/runtime/script_runtime.hpp"

namespace vnmaki {

class Audio;
class DialoguePresenter;
class Platform;
class Playback;
class SelfVoicing;

/// The defaults are the ones of `prefs` in the prelude.
struct Preferences {
  /// Characters a second.
  double text_speed = 40;
  /// Seconds auto-advance waits after a line.
  double auto_delay = 1;
  double volume_music = 1;
  double volume_sound = 1;
  double volume_voice = 1;
  double volume_ambient = 1;
  double volume_ui = 1;
  bool fullscreen = false;
  /// The skip key skips text the player has not read; off, only text read before.
  bool skip_unseen = false;
  double window_opacity = 1;
  /// Read by the presentation code (camera moves, flashes) that reduce_motion switches off.
  bool reduce_motion = false;
  /// Speak the story and the focused UI node (SelfVoicing).
  bool self_voicing = false;

  bool operator==(const Preferences&) const = default;
};

/// The range each preference is clamped to.
inline constexpr double kMinTextSpeed = 1;
inline constexpr double kMaxTextSpeed = 1000;
inline constexpr double kMaxAutoDelay = 60;
inline constexpr double kMinWindowOpacity = 0.3;

/// Reads `prefs` from `runtime`, clamped; a value of the wrong type reads as the default.
[[nodiscard]] Preferences read_preferences(const ScriptRuntime& runtime);

/// What the preferences change. A null target is skipped.
struct PreferenceTargets {
  DialoguePresenter* presenter = nullptr;
  Playback* playback = nullptr;
  Audio* audio = nullptr;
  Platform* platform = nullptr;
  SelfVoicing* self_voicing = nullptr;
};

class PreferenceApplier {
 public:
  /// `runtime` must outlive this. Nothing is applied until update(); the targets start out with
  /// the default preferences.
  PreferenceApplier(const ScriptRuntime& runtime, PreferenceTargets targets);

  /// Applies the preferences that changed since the last call. Call it once a frame.
  void update();

  /// What was applied last.
  [[nodiscard]] const Preferences& current() const { return applied_; }

 private:
  const ScriptRuntime* runtime_;
  PreferenceTargets targets_;
  Preferences applied_{};
};

}  // namespace vnmaki
