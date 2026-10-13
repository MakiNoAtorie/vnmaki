#include "engine/platform/sdl_platform.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include <SDL3/SDL.h>

#include "engine/platform/sdl_events.hpp"

namespace vnmaki {

namespace {

constexpr SDL_InitFlags kSubsystems = SDL_INIT_VIDEO | SDL_INIT_GAMEPAD;

[[noreturn]] void throw_sdl_error(const std::string& what) {
  throw PlatformError{what + ": " + SDL_GetError()};
}

}  // namespace

void SdlPlatform::WindowCloser::operator()(SDL_Window* window) const { SDL_DestroyWindow(window); }

void SdlPlatform::GamepadCloser::operator()(SDL_Gamepad* gamepad) const {
  SDL_CloseGamepad(gamepad);
}

SdlPlatform::SdlSubsystems::SdlSubsystems() {
  if (!SDL_InitSubSystem(kSubsystems)) {
    throw_sdl_error("cannot start SDL video and gamepad support");
  }
}

SdlPlatform::SdlSubsystems::~SdlSubsystems() {
  SDL_QuitSubSystem(kSubsystems);
  // Release SDL's remaining global state once nothing else (audio, say) still uses SDL.
  if (SDL_WasInit(0) == 0) {
    SDL_Quit();
  }
}

SdlPlatform::SdlPlatform(const WindowConfig& config)
    : window_(SDL_CreateWindow(config.title.c_str(), config.size.width, config.size.height,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY)) {
  if (!window_) {
    throw_sdl_error("cannot open the window");
  }
}

SdlPlatform::~SdlPlatform() = default;

WindowMetrics SdlPlatform::window_metrics() const {
  WindowMetrics metrics;
  SDL_GetWindowSize(window_.get(), &metrics.points.width, &metrics.points.height);
  SDL_GetWindowSizeInPixels(window_.get(), &metrics.pixels.width, &metrics.pixels.height);
  const float display_scale = SDL_GetWindowDisplayScale(window_.get());
  metrics.display_scale = display_scale > 0 ? display_scale : 1;
  return metrics;
}

std::optional<InputEvent> SdlPlatform::poll_event() {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    // SDL reports buttons and axes only for gamepads that are open.
    if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
      if (SDL_Gamepad* gamepad = SDL_OpenGamepad(event.gdevice.which)) {
        gamepads_.emplace_back(gamepad);
      }
    } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
      std::erase_if(gamepads_, [&](const auto& gamepad) {
        return SDL_GetGamepadID(gamepad.get()) == event.gdevice.which;
      });
    }
    if (auto translated = translate_sdl_event(event, window_metrics())) {
      return translated;
    }
  }
  return std::nullopt;
}

void SdlPlatform::set_text_input(bool enabled) {
  if (enabled) {
    SDL_StartTextInput(window_.get());
  } else {
    SDL_StopTextInput(window_.get());
  }
}

SDL_Window* SdlPlatform::sdl_window() const { return window_.get(); }

}  // namespace vnmaki
