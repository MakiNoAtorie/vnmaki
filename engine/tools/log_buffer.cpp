#include "engine/tools/log_buffer.hpp"

#include <string>
#include <string_view>

namespace vnmaki {

void LogBuffer::add(LogLevel level, std::string_view text) {
  entries_.push_back({.level = level, .text = std::string{text}});
  ++total_;
  while (entries_.size() > capacity_) {
    entries_.pop_front();
  }
}

}  // namespace vnmaki
