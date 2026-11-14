#pragma once

// The player inputs that roll the story back (docs/SPEC.md section 8). The keys and buttons come
// from the InputMap (task 42); the mouse wheel always rolls back.

#include "engine/platform/input.hpp"
#include "engine/platform/input_map.hpp"

namespace vnmaki {

/// True for the mouse wheel turned up, and for a press of a key or gamepad button of
/// InputAction::kRollback in `map` (the default: Page Up, not a repeat, and the left shoulder
/// button): the host then calls ScriptRuntime::roll_back().
[[nodiscard]] bool is_rollback_event(const InputEvent& event, const InputMap& map);
/// With the default map.
[[nodiscard]] bool is_rollback_event(const InputEvent& event);

}  // namespace vnmaki
