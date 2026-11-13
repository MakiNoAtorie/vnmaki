#pragma once

// The player inputs that roll the story back (docs/SPEC.md section 8). Task 42 will make them
// configurable.

#include "engine/platform/input.hpp"

namespace vnmaki {

/// True for the mouse wheel turned up, the Page Up key (not a repeat) and the left shoulder
/// button of a gamepad: the host then calls ScriptRuntime::roll_back().
[[nodiscard]] bool is_rollback_event(const InputEvent& event);

}  // namespace vnmaki
