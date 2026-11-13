#pragma once

// Watches the project folder for changes (docs/SPEC.md section 18). efsw reports the events on
// its own thread; poll() gives them to the main thread as batches.

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace vnmaki {

/// Events that come within this time of each other are one batch.
inline constexpr std::chrono::milliseconds kWatchBatchWindow{100};

class FileWatcher {
 public:
  using Clock = std::chrono::steady_clock;

  /// No operating system watch: only notify() adds events. For tests.
  explicit FileWatcher(std::chrono::milliseconds window = kWatchBatchWindow);
  /// Watches `root` and the folders under it with efsw. Throws std::runtime_error if it cannot.
  explicit FileWatcher(const std::filesystem::path& root,
                       std::chrono::milliseconds window = kWatchBatchWindow);
  FileWatcher(const FileWatcher&) = delete;
  FileWatcher(FileWatcher&&) = delete;
  FileWatcher& operator=(const FileWatcher&) = delete;
  FileWatcher& operator=(FileWatcher&&) = delete;
  ~FileWatcher();

  /// Adds a changed file: its path from the root with `/` separators. Any thread.
  void notify(std::string path, Clock::time_point at = Clock::now());

  /// The paths of one batch, sorted and each once, when no event came for the window; otherwise
  /// empty. Main thread.
  [[nodiscard]] std::vector<std::string> poll(Clock::time_point now = Clock::now());

 private:
  class Os;

  std::chrono::milliseconds window_;
  std::mutex mutex_;
  std::set<std::string> pending_;
  Clock::time_point last_event_{};
  std::unique_ptr<Os> os_;  // declared last: it stops the efsw thread before the members above go
};

}  // namespace vnmaki
