#include "engine/platform/main_loop.hpp"

#include <algorithm>
#include <stdexcept>
#include <thread>
#include <variant>

namespace vnmaki {

Duration SteadyClock::now() {
  return std::chrono::duration_cast<Duration>(std::chrono::steady_clock::now().time_since_epoch());
}

void SteadyClock::sleep_for(Duration duration) { std::this_thread::sleep_for(duration); }

MainLoop::MainLoop(Platform& platform, Clock& clock, MainLoopConfig config)
    : platform_(platform), clock_(clock), config_(config), previous_time_(clock.now()) {
  if (config_.update_step <= Duration::zero()) {
    throw std::invalid_argument{"MainLoopConfig::update_step must be positive"};
  }
  if (config_.max_updates_per_frame < 1) {
    throw std::invalid_argument{"MainLoopConfig::max_updates_per_frame must be at least 1"};
  }
}

bool MainLoop::run_frame(const MainLoopHooks& hooks) {
  const Duration frame_start = clock_.now();
  // A clock that went backwards adds nothing rather than negative time.
  accumulated_ += std::max(frame_start - previous_time_, Duration::zero());
  previous_time_ = frame_start;

  while (auto event = platform_.poll_event()) {
    if (hooks.on_event) {
      hooks.on_event(*event);
    }
    if (std::holds_alternative<QuitRequestedEvent>(*event)) {
      quit_requested_ = true;
    }
  }
  if (quit_requested_) {
    return false;
  }

  const Duration step = config_.update_step;
  for (int updates = 0; accumulated_ >= step; ++updates) {
    if (updates == config_.max_updates_per_frame) {
      accumulated_ %= step;  // Drop the whole steps of the backlog, keep the fraction.
      break;
    }
    if (hooks.update) {
      hooks.update(step);
    }
    accumulated_ -= step;
  }

  if (hooks.draw) {
    const double alpha = std::chrono::duration<double>(accumulated_) / step;
    hooks.draw(alpha);
  }

  if (config_.min_frame_time > Duration::zero()) {
    const Duration frame_time = clock_.now() - frame_start;
    if (frame_time < config_.min_frame_time) {
      clock_.sleep_for(config_.min_frame_time - frame_time);
    }
  }
  return !quit_requested_;
}

std::uint64_t MainLoop::run(const MainLoopHooks& hooks, std::optional<std::uint64_t> max_frames) {
  std::uint64_t frames = 0;
  while (!quit_requested_ && (!max_frames || frames < *max_frames)) {
    run_frame(hooks);
    ++frames;
  }
  return frames;
}

void MainLoop::request_quit() { quit_requested_ = true; }

bool MainLoop::quit_requested() const { return quit_requested_; }

}  // namespace vnmaki
