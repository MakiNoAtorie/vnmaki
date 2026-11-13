#include "engine/dialogue/rollback_input.hpp"

#include <variant>

namespace vnmaki {

bool is_rollback_event(const InputEvent& event) {
  if (const auto* wheel = std::get_if<WheelEvent>(&event)) {
    return wheel->dy > 0;
  }
  if (const auto* key = std::get_if<KeyEvent>(&event)) {
    return key->pressed && !key->repeat && key->key == Key::kPageUp;
  }
  if (const auto* button = std::get_if<GamepadButtonEvent>(&event)) {
    return button->pressed && button->button == GamepadButton::kLeftShoulder;
  }
  return false;
}

}  // namespace vnmaki
