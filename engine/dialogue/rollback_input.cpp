#include "engine/dialogue/rollback_input.hpp"

#include <variant>

namespace vnmaki {

bool is_rollback_event(const InputEvent& event, const InputMap& map) {
  if (const auto* wheel = std::get_if<WheelEvent>(&event)) {
    return wheel->dy > 0;
  }
  return map.matches(InputAction::kRollback, event);
}

bool is_rollback_event(const InputEvent& event) {
  static const InputMap kDefaults;
  return is_rollback_event(event, kDefaults);
}

}  // namespace vnmaki
