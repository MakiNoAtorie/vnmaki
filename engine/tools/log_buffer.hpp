#pragma once

// The log the dev console shows: Lua `print`, script warnings and what the engine reports.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

#include "engine/runtime/script_runtime.hpp"

namespace vnmaki {

enum class LogLevel : std::uint8_t { kInfo, kWarning, kError };

struct LogEntry {
  LogLevel level = LogLevel::kInfo;
  std::string text{};
};

/// The last `capacity` entries. As a ScriptObserver it logs `print` as info and dev-mode
/// warnings as warnings. Use it from the main thread only.
class LogBuffer final : public ScriptObserver {
 public:
  static constexpr std::size_t kDefaultCapacity = 1000;

  explicit LogBuffer(std::size_t capacity = kDefaultCapacity) : capacity_(capacity) {}

  void add(LogLevel level, std::string_view text);
  void clear() { entries_.clear(); }
  [[nodiscard]] const std::deque<LogEntry>& entries() const { return entries_; }
  /// How many entries were added since the start, including those that fell out.
  [[nodiscard]] std::size_t total() const { return total_; }

  void on_print(std::string_view text) override { add(LogLevel::kInfo, text); }
  void on_warning(std::string_view message) override { add(LogLevel::kWarning, message); }

 private:
  std::size_t capacity_;
  std::size_t total_ = 0;
  std::deque<LogEntry> entries_;
};

}  // namespace vnmaki
