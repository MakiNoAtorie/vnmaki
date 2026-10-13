// Translating SDL events into engine InputEvents. SDL_Event is a plain struct, so no window or
// SDL_Init is needed: each test builds the event SDL would deliver and checks the translation.

#include "engine/platform/sdl_events.hpp"

#include <array>
#include <optional>
#include <utility>

#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include "engine/platform/input.hpp"
#include "engine/platform/viewport.hpp"

using vnmaki::InputEvent;
using vnmaki::Key;

namespace {

const vnmaki::WindowMetrics kStandard{.points = {1280, 720}, .pixels = {1280, 720}};
const vnmaki::WindowMetrics kRetina{.points = {1280, 720}, .pixels = {2560, 1440}};

SDL_Event key_event(SDL_EventType type, SDL_Keycode key, SDL_Keymod mod = SDL_KMOD_NONE,
                    bool repeat = false) {
  SDL_Event event{};
  event.key.type = type;
  event.key.key = key;
  event.key.mod = mod;
  event.key.down = type == SDL_EVENT_KEY_DOWN;
  event.key.repeat = repeat;
  return event;
}

SDL_Event of_type(SDL_EventType type) {
  SDL_Event event{};
  event.type = type;
  return event;
}

std::optional<InputEvent> translate(const SDL_Event& event,
                                    const vnmaki::WindowMetrics& metrics = kStandard) {
  return vnmaki::translate_sdl_event(event, metrics);
}

std::optional<InputEvent> some(InputEvent event) { return event; }

}  // namespace

TEST_CASE("sdl events: a key press becomes a KeyEvent with its modifiers") {
  CHECK(translate(key_event(SDL_EVENT_KEY_DOWN, SDLK_Z, SDL_KMOD_LSHIFT)) ==
        some(vnmaki::KeyEvent{.key = Key::kZ, .pressed = true, .mods = {.shift = true}}));
  CHECK(translate(key_event(SDL_EVENT_KEY_UP, SDLK_RETURN, SDL_KMOD_RCTRL | SDL_KMOD_LALT)) ==
        some(vnmaki::KeyEvent{.key = Key::kEnter, .mods = {.ctrl = true, .alt = true}}));
  CHECK(translate(key_event(SDL_EVENT_KEY_DOWN, SDLK_S, SDL_KMOD_LGUI)) ==
        some(vnmaki::KeyEvent{.key = Key::kS, .pressed = true, .mods = {.gui = true}}));
}

TEST_CASE("sdl events: auto-repeat presses are marked as repeats") {
  CHECK(translate(key_event(SDL_EVENT_KEY_DOWN, SDLK_SPACE, SDL_KMOD_NONE, true)) ==
        some(vnmaki::KeyEvent{.key = Key::kSpace, .pressed = true, .repeat = true}));
}

TEST_CASE("sdl events: letters, digits, function and navigation keys map to engine keys") {
  const auto cases = std::to_array<std::pair<SDL_Keycode, Key>>({
      {SDLK_A, Key::kA},
      {SDLK_M, Key::kM},
      {SDLK_0, Key::kDigit0},
      {SDLK_9, Key::kDigit9},
      {SDLK_F1, Key::kF1},
      {SDLK_F12, Key::kF12},
      {SDLK_ESCAPE, Key::kEscape},
      {SDLK_KP_ENTER, Key::kKeypadEnter},
      {SDLK_TAB, Key::kTab},
      {SDLK_BACKSPACE, Key::kBackspace},
      {SDLK_DELETE, Key::kDelete},
      {SDLK_INSERT, Key::kInsert},
      {SDLK_HOME, Key::kHome},
      {SDLK_END, Key::kEnd},
      {SDLK_PAGEUP, Key::kPageUp},
      {SDLK_PAGEDOWN, Key::kPageDown},
      {SDLK_LEFT, Key::kLeft},
      {SDLK_RIGHT, Key::kRight},
      {SDLK_UP, Key::kUp},
      {SDLK_DOWN, Key::kDown},
      {SDLK_LSHIFT, Key::kLeftShift},
      {SDLK_RSHIFT, Key::kRightShift},
      {SDLK_LCTRL, Key::kLeftCtrl},
      {SDLK_RCTRL, Key::kRightCtrl},
      {SDLK_LALT, Key::kLeftAlt},
      {SDLK_RALT, Key::kRightAlt},
      {SDLK_LGUI, Key::kLeftGui},
      {SDLK_RGUI, Key::kRightGui},
  });
  for (const auto& [sdl_key, key] : cases) {
    CAPTURE(sdl_key);
    CHECK(translate(key_event(SDL_EVENT_KEY_DOWN, sdl_key)) ==
          some(vnmaki::KeyEvent{.key = key, .pressed = true}));
  }
}

TEST_CASE("sdl events: keys the engine does not know produce no event") {
  CHECK(translate(key_event(SDL_EVENT_KEY_DOWN, SDLK_VOLUMEUP)) == std::nullopt);
  CHECK(translate(key_event(SDL_EVENT_KEY_DOWN, SDLK_UNKNOWN)) == std::nullopt);
}

TEST_CASE("sdl events: typed text becomes a TextInputEvent") {
  SDL_Event event = of_type(SDL_EVENT_TEXT_INPUT);
  event.text.text = "\xe3\x81\x82";  // Hiragana "a", as an input method would commit it.

  CHECK(translate(event) == some(vnmaki::TextInputEvent{.text = "\xe3\x81\x82"}));
}

TEST_CASE("sdl events: pointer positions are converted from window points to pixels") {
  SDL_Event motion = of_type(SDL_EVENT_MOUSE_MOTION);
  constexpr vnmaki::PointF kInPoints{.x = 100, .y = 50};
  constexpr vnmaki::PointF kInRetinaPixels{.x = 200, .y = 100};
  motion.motion.x = kInPoints.x;
  motion.motion.y = kInPoints.y;

  CHECK(translate(motion, kStandard) == some(vnmaki::PointerMoveEvent{.position = kInPoints}));
  CHECK(translate(motion, kRetina) == some(vnmaki::PointerMoveEvent{.position = kInRetinaPixels}));
}

TEST_CASE("sdl events: mouse buttons become PointerButtonEvents with click counts") {
  SDL_Event press = of_type(SDL_EVENT_MOUSE_BUTTON_DOWN);
  press.button.button = SDL_BUTTON_RIGHT;
  press.button.down = true;
  press.button.clicks = 2;
  constexpr vnmaki::PointF kInPoints{.x = 10, .y = 20};
  constexpr vnmaki::PointF kInRetinaPixels{.x = 20, .y = 40};
  press.button.x = kInPoints.x;
  press.button.y = kInPoints.y;

  CHECK(translate(press, kRetina) ==
        some(vnmaki::PointerButtonEvent{.button = vnmaki::MouseButton::kRight,
                                        .pressed = true,
                                        .position = kInRetinaPixels,
                                        .clicks = 2}));

  SDL_Event release = of_type(SDL_EVENT_MOUSE_BUTTON_UP);
  release.button.button = SDL_BUTTON_X2;
  release.button.clicks = 1;
  CHECK(translate(release) ==
        some(vnmaki::PointerButtonEvent{.button = vnmaki::MouseButton::kX2, .clicks = 1}));
}

TEST_CASE("sdl events: the mouse wheel becomes a WheelEvent") {
  SDL_Event wheel = of_type(SDL_EVENT_MOUSE_WHEEL);
  constexpr float kHalfNotchRight = 0.5F;
  wheel.wheel.x = kHalfNotchRight;
  wheel.wheel.y = -2;

  CHECK(translate(wheel) == some(vnmaki::WheelEvent{.dx = kHalfNotchRight, .dy = -2}));
}

TEST_CASE("sdl events: gamepad connection, buttons and axes become gamepad events") {
  constexpr SDL_JoystickID kPad = 7;

  SDL_Event added = of_type(SDL_EVENT_GAMEPAD_ADDED);
  added.gdevice.which = kPad;
  CHECK(translate(added) == some(vnmaki::GamepadConnectedEvent{.id = kPad}));

  SDL_Event removed = of_type(SDL_EVENT_GAMEPAD_REMOVED);
  removed.gdevice.which = kPad;
  CHECK(translate(removed) == some(vnmaki::GamepadDisconnectedEvent{.id = kPad}));

  SDL_Event button = of_type(SDL_EVENT_GAMEPAD_BUTTON_DOWN);
  button.gbutton.which = kPad;
  button.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
  button.gbutton.down = true;
  CHECK(translate(button) ==
        some(vnmaki::GamepadButtonEvent{
            .id = kPad, .button = vnmaki::GamepadButton::kSouth, .pressed = true}));

  button.type = SDL_EVENT_GAMEPAD_BUTTON_UP;
  button.gbutton.button = SDL_GAMEPAD_BUTTON_DPAD_LEFT;
  button.gbutton.down = false;
  CHECK(translate(button) ==
        some(vnmaki::GamepadButtonEvent{.id = kPad, .button = vnmaki::GamepadButton::kDpadLeft}));
}

TEST_CASE("sdl events: gamepad sticks scale to [-1, 1] and triggers to [0, 1]") {
  SDL_Event axis = of_type(SDL_EVENT_GAMEPAD_AXIS_MOTION);
  axis.gaxis.which = 1;

  const auto axis_value = [&](SDL_GamepadAxis sdl_axis, Sint16 value) {
    axis.gaxis.axis = static_cast<Uint8>(sdl_axis);
    axis.gaxis.value = value;
    const auto translated = translate(axis);
    REQUIRE(translated);
    REQUIRE(std::holds_alternative<vnmaki::GamepadAxisEvent>(*translated));
    return std::get<vnmaki::GamepadAxisEvent>(*translated);
  };

  CHECK(axis_value(SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX).value == doctest::Approx(1));
  CHECK(axis_value(SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MIN).value == doctest::Approx(-1));
  CHECK(axis_value(SDL_GAMEPAD_AXIS_RIGHTY, 0).value == doctest::Approx(0));
  CHECK(axis_value(SDL_GAMEPAD_AXIS_RIGHTY, 0).axis == vnmaki::GamepadAxis::kRightY);
  CHECK(axis_value(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX).value ==
        doctest::Approx(1));
  CHECK(axis_value(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0).axis == vnmaki::GamepadAxis::kLeftTrigger);
}

TEST_CASE("sdl events: window resize, focus and close map to window events") {
  SDL_Event resized = of_type(SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED);
  resized.window.data1 = kRetina.pixels.width;
  resized.window.data2 = kRetina.pixels.height;
  CHECK(translate(resized) == some(vnmaki::WindowResizedEvent{.pixels = kRetina.pixels}));

  CHECK(translate(of_type(SDL_EVENT_WINDOW_FOCUS_GAINED)) ==
        some(vnmaki::WindowFocusEvent{.focused = true}));
  CHECK(translate(of_type(SDL_EVENT_WINDOW_FOCUS_LOST)) ==
        some(vnmaki::WindowFocusEvent{.focused = false}));

  CHECK(translate(of_type(SDL_EVENT_WINDOW_CLOSE_REQUESTED)) == some(vnmaki::QuitRequestedEvent{}));
  CHECK(translate(of_type(SDL_EVENT_QUIT)) == some(vnmaki::QuitRequestedEvent{}));
}

TEST_CASE("sdl events: events the engine does not use produce nothing") {
  CHECK(translate(of_type(SDL_EVENT_CLIPBOARD_UPDATE)) == std::nullopt);
  // The logical-size resize is ignored: the pixel-size event that follows carries what matters.
  CHECK(translate(of_type(SDL_EVENT_WINDOW_RESIZED)) == std::nullopt);
}
