#include "engine/platform/platform.hpp"

#include <utility>
#include <variant>

namespace vnmaki {

HeadlessPlatform::HeadlessPlatform(Size window_size)
    : metrics_{.points = window_size, .pixels = window_size} {}

WindowMetrics HeadlessPlatform::window_metrics() const { return metrics_; }

std::optional<InputEvent> HeadlessPlatform::poll_event() {
  if (events_.empty()) {
    return std::nullopt;
  }
  InputEvent event = std::move(events_.front());
  events_.pop_front();
  return event;
}

void HeadlessPlatform::set_text_input(bool enabled) { text_input_ = enabled; }

void HeadlessPlatform::push_event(InputEvent event) {
  if (const auto* resized = std::get_if<WindowResizedEvent>(&event)) {
    metrics_.points = resized->pixels;
    metrics_.pixels = resized->pixels;
  }
  events_.push_back(std::move(event));
}

bool HeadlessPlatform::text_input_enabled() const { return text_input_; }

}  // namespace vnmaki
