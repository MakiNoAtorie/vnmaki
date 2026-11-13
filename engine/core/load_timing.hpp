#pragma once

// Asset load timing for the profiler (docs/SPEC.md section 18). Loaders of images, fonts, audio and
// movies hold a LoadTimer; with no sink set it costs one atomic read.

#include <atomic>
#include <chrono>
#include <functional>
#include <string_view>

namespace vnmaki {

using LoadSink = std::function<void(std::string_view path, std::chrono::nanoseconds time)>;

/// Sets where finished loads go. An empty function removes the sink. Loads run on job threads, so
/// the sink must be thread-safe; set_load_sink() waits for a call in progress.
void set_load_sink(LoadSink sink);

/// Times the load of `path` from construction to destruction (also when it throws). `path` must
/// outlive the timer.
class LoadTimer {
 public:
  explicit LoadTimer(std::string_view path);
  ~LoadTimer();
  LoadTimer(const LoadTimer&) = delete;
  LoadTimer& operator=(const LoadTimer&) = delete;
  LoadTimer(LoadTimer&&) = delete;
  LoadTimer& operator=(LoadTimer&&) = delete;

 private:
  std::string_view path_;
  std::chrono::steady_clock::time_point start_{};
  bool active_;
};

}  // namespace vnmaki
