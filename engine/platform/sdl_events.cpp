#include "engine/platform/sdl_events.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace vnmaki {

namespace {

/// The key `first + (code - first_code)`, for runs of consecutive keycodes and Key values.
Key key_in_run(Key first, SDL_Keycode first_code, SDL_Keycode code) {
  return static_cast<Key>(static_cast<std::uint8_t>(first) + (code - first_code));
}

std::optional<Key> key_from_sdl(SDL_Keycode code) {
  if (code >= SDLK_A && code <= SDLK_Z) {
    return key_in_run(Key::kA, SDLK_A, code);
  }
  if (code >= SDLK_0 && code <= SDLK_9) {
    return key_in_run(Key::kDigit0, SDLK_0, code);
  }
  if (code >= SDLK_F1 && code <= SDLK_F12) {
    return key_in_run(Key::kF1, SDLK_F1, code);
  }
  switch (code) {
    case SDLK_ESCAPE:
      return Key::kEscape;
    case SDLK_RETURN:
      return Key::kEnter;
    case SDLK_KP_ENTER:
      return Key::kKeypadEnter;
    case SDLK_SPACE:
      return Key::kSpace;
    case SDLK_TAB:
      return Key::kTab;
    case SDLK_BACKSPACE:
      return Key::kBackspace;
    case SDLK_DELETE:
      return Key::kDelete;
    case SDLK_INSERT:
      return Key::kInsert;
    case SDLK_HOME:
      return Key::kHome;
    case SDLK_END:
      return Key::kEnd;
    case SDLK_PAGEUP:
      return Key::kPageUp;
    case SDLK_PAGEDOWN:
      return Key::kPageDown;
    case SDLK_LEFT:
      return Key::kLeft;
    case SDLK_RIGHT:
      return Key::kRight;
    case SDLK_UP:
      return Key::kUp;
    case SDLK_DOWN:
      return Key::kDown;
    case SDLK_LSHIFT:
      return Key::kLeftShift;
    case SDLK_RSHIFT:
      return Key::kRightShift;
    case SDLK_LCTRL:
      return Key::kLeftCtrl;
    case SDLK_RCTRL:
      return Key::kRightCtrl;
    case SDLK_LALT:
      return Key::kLeftAlt;
    case SDLK_RALT:
      return Key::kRightAlt;
    case SDLK_LGUI:
      return Key::kLeftGui;
    case SDLK_RGUI:
      return Key::kRightGui;
    default:
      return std::nullopt;
  }
}

KeyMods mods_from_sdl(SDL_Keymod mod) {
  return {.shift = (mod & SDL_KMOD_SHIFT) != 0,
          .ctrl = (mod & SDL_KMOD_CTRL) != 0,
          .alt = (mod & SDL_KMOD_ALT) != 0,
          .gui = (mod & SDL_KMOD_GUI) != 0};
}

std::optional<MouseButton> mouse_button_from_sdl(Uint8 button) {
  switch (button) {
    case SDL_BUTTON_LEFT:
      return MouseButton::kLeft;
    case SDL_BUTTON_MIDDLE:
      return MouseButton::kMiddle;
    case SDL_BUTTON_RIGHT:
      return MouseButton::kRight;
    case SDL_BUTTON_X1:
      return MouseButton::kX1;
    case SDL_BUTTON_X2:
      return MouseButton::kX2;
    default:
      return std::nullopt;
  }
}

std::optional<GamepadButton> gamepad_button_from_sdl(Uint8 button) {
  switch (static_cast<SDL_GamepadButton>(button)) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
      return GamepadButton::kSouth;
    case SDL_GAMEPAD_BUTTON_EAST:
      return GamepadButton::kEast;
    case SDL_GAMEPAD_BUTTON_WEST:
      return GamepadButton::kWest;
    case SDL_GAMEPAD_BUTTON_NORTH:
      return GamepadButton::kNorth;
    case SDL_GAMEPAD_BUTTON_BACK:
      return GamepadButton::kBack;
    case SDL_GAMEPAD_BUTTON_GUIDE:
      return GamepadButton::kGuide;
    case SDL_GAMEPAD_BUTTON_START:
      return GamepadButton::kStart;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
      return GamepadButton::kLeftStick;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
      return GamepadButton::kRightStick;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
      return GamepadButton::kLeftShoulder;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
      return GamepadButton::kRightShoulder;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
      return GamepadButton::kDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
      return GamepadButton::kDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
      return GamepadButton::kDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
      return GamepadButton::kDpadRight;
    default:
      return std::nullopt;
  }
}

std::optional<GamepadAxis> gamepad_axis_from_sdl(Uint8 axis) {
  switch (static_cast<SDL_GamepadAxis>(axis)) {
    case SDL_GAMEPAD_AXIS_LEFTX:
      return GamepadAxis::kLeftX;
    case SDL_GAMEPAD_AXIS_LEFTY:
      return GamepadAxis::kLeftY;
    case SDL_GAMEPAD_AXIS_RIGHTX:
      return GamepadAxis::kRightX;
    case SDL_GAMEPAD_AXIS_RIGHTY:
      return GamepadAxis::kRightY;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
      return GamepadAxis::kLeftTrigger;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
      return GamepadAxis::kRightTrigger;
    default:
      return std::nullopt;
  }
}

/// SDL axes run from -32768 to 32767 (triggers from 0); scale so both ends reach exactly 1.
float axis_value_from_sdl(Sint16 value) {
  return std::clamp(static_cast<float>(value) / SDL_JOYSTICK_AXIS_MAX, -1.0F, 1.0F);
}

std::optional<InputEvent> translate_key(const SDL_KeyboardEvent& key) {
  const auto engine_key = key_from_sdl(key.key);
  if (!engine_key) {
    return std::nullopt;
  }
  return KeyEvent{.key = *engine_key,
                  .pressed = key.down,
                  .repeat = key.repeat,
                  .mods = mods_from_sdl(key.mod)};
}

std::optional<InputEvent> translate_mouse_button(const SDL_MouseButtonEvent& button,
                                                 const WindowMetrics& metrics) {
  const auto engine_button = mouse_button_from_sdl(button.button);
  if (!engine_button) {
    return std::nullopt;
  }
  return PointerButtonEvent{.button = *engine_button,
                            .pressed = button.down,
                            .position = points_to_pixels(metrics, {button.x, button.y}),
                            .clicks = button.clicks};
}

std::optional<InputEvent> translate_gamepad_button(const SDL_GamepadButtonEvent& button) {
  const auto engine_button = gamepad_button_from_sdl(button.button);
  if (!engine_button) {
    return std::nullopt;
  }
  return GamepadButtonEvent{.id = button.which, .button = *engine_button, .pressed = button.down};
}

std::optional<InputEvent> translate_gamepad_axis(const SDL_GamepadAxisEvent& axis) {
  const auto engine_axis = gamepad_axis_from_sdl(axis.axis);
  if (!engine_axis) {
    return std::nullopt;
  }
  return GamepadAxisEvent{
      .id = axis.which, .axis = *engine_axis, .value = axis_value_from_sdl(axis.value)};
}

}  // namespace

std::optional<InputEvent> translate_sdl_event(const SDL_Event& event,
                                              const WindowMetrics& metrics) {
  switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      return translate_key(event.key);
    case SDL_EVENT_TEXT_INPUT:
      return TextInputEvent{.text = event.text.text != nullptr ? event.text.text : ""};
    case SDL_EVENT_MOUSE_MOTION:
      return PointerMoveEvent{.position =
                                  points_to_pixels(metrics, {event.motion.x, event.motion.y})};
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      return translate_mouse_button(event.button, metrics);
    case SDL_EVENT_MOUSE_WHEEL:
      return WheelEvent{.dx = event.wheel.x, .dy = event.wheel.y};
    case SDL_EVENT_GAMEPAD_ADDED:
      return GamepadConnectedEvent{.id = event.gdevice.which};
    case SDL_EVENT_GAMEPAD_REMOVED:
      return GamepadDisconnectedEvent{.id = event.gdevice.which};
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      return translate_gamepad_button(event.gbutton);
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      return translate_gamepad_axis(event.gaxis);
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      return WindowResizedEvent{.pixels = {event.window.data1, event.window.data2}};
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      return WindowFocusEvent{.focused = true};
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      return WindowFocusEvent{.focused = false};
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    case SDL_EVENT_QUIT:
      return QuitRequestedEvent{};
    default:
      return std::nullopt;
  }
}

}  // namespace vnmaki
