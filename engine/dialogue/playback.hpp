#pragma once

// Skip and auto-advance (docs/SPEC.md section 8). Playback sits beside a DialoguePresenter: each
// frame, after the presenter's update(), it advances the story by itself while skip or auto is
// on. It records every line the player reaches in the PersistentStore's seen set. It needs no
// window or clock: the caller passes the elapsed seconds.

#include <cstdint>
#include <functional>

#include "engine/runtime/script_runtime.hpp"

namespace vnmaki {

class DialoguePresenter;
class PersistentStore;

struct PlaybackOptions {
  /// Seconds auto-advance waits after a line is fully shown, whatever its length.
  double auto_base_delay = 1.0;
  /// Seconds it waits more for each glyph of the line, times the speaker's `auto_multiplier`.
  double auto_seconds_per_glyph = 0.05;
  /// Seconds between two skipped lines.
  double skip_interval = 0.05;
  /// The hook for voices: auto-advance does not count down while it returns true, and after a
  /// line that had a voice it waits only `auto_base_delay`. Use Audio::voice_playing().
  std::function<bool()> voice_playing{};
};

class Playback {
 public:
  /// All three must outlive this. `presenter` must be made from `runtime`.
  Playback(ScriptRuntime& runtime, DialoguePresenter& presenter, PersistentStore& persistent,
           PlaybackOptions options = {});

  void set_skip(SkipMode mode) { skip_ = mode; }
  void set_auto(bool on);
  [[nodiscard]] SkipMode skip() const { return skip_; }
  [[nodiscard]] bool automatic() const { return auto_; }

  /// Applies the script's `skip` and `auto` calls, notes the line now shown as seen, and
  /// advances the story if skip or auto says so. Skip stops by itself at an unseen line (mode
  /// kSeen) and at any menu or screen.
  void update(double seconds);

  /// The seconds auto-advance waits after the current line is fully shown.
  [[nodiscard]] double auto_delay() const;

 private:
  void observe_line();
  void skip_step();
  void auto_step(double seconds);

  ScriptRuntime* runtime_;
  DialoguePresenter* presenter_;
  PersistentStore* persistent_;
  PlaybackOptions options_;
  SkipMode skip_ = SkipMode::kOff;
  bool auto_ = false;
  std::uint64_t observed_serial_ = 0;
  double skip_timer_ = 0;
  double auto_timer_ = 0;
  bool line_had_voice_ = false;
};

}  // namespace vnmaki
