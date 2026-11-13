#include "engine/core/load_timing.hpp"

#include <mutex>
#include <utility>

namespace vnmaki {

namespace {

std::atomic<bool> g_has_sink{false};
std::mutex g_sink_mutex;
LoadSink g_sink;  // guarded by g_sink_mutex

}  // namespace

void set_load_sink(LoadSink sink) {
  const std::scoped_lock lock{g_sink_mutex};
  g_has_sink = static_cast<bool>(sink);
  g_sink = std::move(sink);
}

LoadTimer::LoadTimer(std::string_view path) : path_(path), active_(g_has_sink.load()) {
  if (active_) {
    start_ = std::chrono::steady_clock::now();
  }
}

LoadTimer::~LoadTimer() {
  if (!active_) {
    return;
  }
  const auto time = std::chrono::steady_clock::now() - start_;
  const std::scoped_lock lock{g_sink_mutex};
  if (g_sink) {
    try {
      g_sink(path_, std::chrono::duration_cast<std::chrono::nanoseconds>(time));
    } catch (...) {  // a destructor must not throw; the profiler loses one load
    }
  }
}

}  // namespace vnmaki
