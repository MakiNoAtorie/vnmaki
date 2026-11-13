#pragma once

// The engine's default screens, as a Lua chunk (docs/SPEC.md section 15). ScriptRuntime::
// load_scripts() runs it before the game's scripts, so a game replaces a screen by defining one
// with the same name.

#include <string_view>

namespace vnmaki {

/// The Lua source of the screens title, say, choice, confirm, save, load, preferences,
/// quick_menu, nvl and gallery. Sizes follow the global `canvas`. A button that returns a string
/// starting with "@" asks for an engine action (engine/ui/screen_actions.hpp).
[[nodiscard]] std::string_view default_screens_source();

}  // namespace vnmaki
