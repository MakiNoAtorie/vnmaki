#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

#include "engine/platform/input.hpp"
#include "engine/platform/platform.hpp"

namespace vnmaki {

using Duration = std::chrono::nanoseconds;

/// Where the main loop gets the time. Tests inject a fake one.
class Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  Clock(Clock&&) = delete;
  Clock& operator=(Clock&&) = delete;
  virtual ~Clock() = default;

  /// Monotonic time since an arbitrary epoch.
  [[nodiscard]] virtual Duration now() = 0;
  virtual void sleep_for(Duration duration) = 0;
};

/// The real clock: std::chrono::steady_clock.
class SteadyClock final : public Clock {
 public:
  [[nodiscard]] Duration now() override;
  void sleep_for(Duration duration) override;
};

struct MainLoopConfig {
  static constexpr int kDefaultUpdatesPerSecond = 60;
  static constexpr int kDefaultMaxUpdatesPerFrame = 8;

  /// Simulated time per update. Game logic always advances by exactly this much.
  Duration update_step =
      std::chrono::duration_cast<Duration>(std::chrono::seconds{1}) / kDefaultUpdatesPerSecond;
  /// After a stall (a debugger pause, a slow load), at most this many updates run in one frame;
  /// the rest of the backlog is dropped so the game slows down instead of freezing.
  int max_updates_per_frame = kDefaultMaxUpdatesPerFrame;
  /// When nonzero, a frame that finishes early sleeps until this much time has passed. For when
  /// nothing else (such as vsync) paces the loop.
  Duration min_frame_time{0};
};

/// What the game does each frame. Empty hooks are skipped.
struct MainLoopHooks {
  /// Called for each platform event, at the start of the frame.
  std::function<void(const InputEvent&)> on_event{};
  /// Called zero or more times per frame, once per fixed step of elapsed time.
  std::function<void(Duration step)> update{};
  /// Called once per frame after the updates. `alpha` in [0, 1) is how far real time has run
  /// past the last update, as a fraction of a step, for interpolating motion.
  std::function<void(double alpha)> draw{};
};

/// Runs frames: pending events, then fixed-timestep updates, then one draw. A
/// QuitRequestedEvent ends the loop once it has been delivered.
class MainLoop {
 public:
  /// Time starts counting at construction. Both references must outlive the loop.
  MainLoop(Platform& platform, Clock& clock, MainLoopConfig config = {});

  /// Runs one frame. Returns false once quit has been requested; the frame then skips its
  /// updates and draw.
  bool run_frame(const MainLoopHooks& hooks);

  /// Runs frames until quit is requested or `max_frames` frames have run. Returns the number
  /// of frames that ran.
  std::uint64_t run(const MainLoopHooks& hooks, std::optional<std::uint64_t> max_frames = {});

  /// Ends the loop after the current frame (from a hook, say).
  void request_quit();
  [[nodiscard]] bool quit_requested() const;

 private:
  Platform* platform_;
  Clock* clock_;
  MainLoopConfig config_;
  Duration previous_time_;
  Duration accumulated_{0};
  bool quit_requested_ = false;
};

}  // namespace vnmaki
