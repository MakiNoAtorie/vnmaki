#pragma once

// Internal to the platform module: the one header that exposes SDL types. Only engine/platform
// sources and tests include it.

#include <optional>

#include <SDL3/SDL_events.h>

#include "engine/platform/input.hpp"
#include "engine/platform/viewport.hpp"

namespace vnmaki {

/// Translates one SDL event into an engine event, or nullopt for events the engine does not
/// use (and keys or buttons it has no name for). `metrics` is the window's current size, used to
/// convert pointer positions from points to pixels.
[[nodiscard]] std::optional<InputEvent> translate_sdl_event(const SDL_Event& event,
                                                            const WindowMetrics& metrics);

}  // namespace vnmaki
