#pragma once

// The wait presenter (docs/SPEC.md section 10): ends the script's `wait(tag)` and `wait_all()`
// steps. A wait is always a step, also when there is nothing to wait for, so that a replay counts
// the same steps as live play; this class goes past it as soon as its tweens have ended. A
// transition (engine/anim/transition.hpp) is a wait for the tag kTransitionTag, and `pause(seconds)`
// is one for kPauseTag, which ends by time.

namespace vnmaki {

class SceneCommands;
class ScriptRuntime;

/// Drives a started ScriptRuntime through its wait steps. Both objects must outlive it.
class WaitPresenter {
 public:
  WaitPresenter(ScriptRuntime& runtime, SceneCommands& scene);

  /// True while the story waits at a `wait` or `wait_all`.
  [[nodiscard]] bool waiting() const;

  /// Call once per update, after the scene moved its tweens, with the seconds that passed. If the
  /// story waits at a wait whose tweens have ended, or a `pause` that has run its time, the script
  /// moves on. At most one wait ends in a call. Returns true if it did.
  bool update(double seconds = 0);

  /// The player clicked, or skip mode is on. If the story waits at a wait, its tweens jump to
  /// their end values and the script moves on. Returns true if it did; the caller then does not
  /// treat the input as an advance.
  bool skip();

 private:
  ScriptRuntime* runtime_;
  SceneCommands* scene_;
  double pause_elapsed_ = 0;  // seconds the current `pause` has run
};

}  // namespace vnmaki
