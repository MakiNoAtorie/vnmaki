#pragma once

// Platform-neutral input events. The platform backend (SDL today) translates its own events into
// these; nothing outside engine/platform sees SDL types.

#include <cstdint>
#include <string>
#include <variant>

#include "engine/platform/geometry.hpp"

namespace vnmaki {

/// A key, by the character or function it produces in the current keyboard layout (so kZ is
/// the key labelled Z, wherever it sits). Keys not listed here produce no event.
enum class Key : std::uint8_t {
  kA,
  kB,
  kC,
  kD,
  kE,
  kF,
  kG,
  kH,
  kI,
  kJ,
  kK,
  kL,
  kM,
  kN,
  kO,
  kP,
  kQ,
  kR,
  kS,
  kT,
  kU,
  kV,
  kW,
  kX,
  kY,
  kZ,
  kDigit0,
  kDigit1,
  kDigit2,
  kDigit3,
  kDigit4,
  kDigit5,
  kDigit6,
  kDigit7,
  kDigit8,
  kDigit9,
  kF1,
  kF2,
  kF3,
  kF4,
  kF5,
  kF6,
  kF7,
  kF8,
  kF9,
  kF10,
  kF11,
  kF12,
  kEscape,
  kEnter,
  kKeypadEnter,
  kSpace,
  kTab,
  kBackspace,
  kDelete,
  kInsert,
  kHome,
  kEnd,
  kPageUp,
  kPageDown,
  kLeft,
  kRight,
  kUp,
  kDown,
  kLeftShift,
  kRightShift,
  kLeftCtrl,
  kRightCtrl,
  kLeftAlt,
  kRightAlt,
  kLeftGui,
  kRightGui,
};

/// Modifier keys held during a key event. Gui is the Windows/Command/Super key.
struct KeyMods {
  bool shift = false;
  bool ctrl = false;
  bool alt = false;
  bool gui = false;

  bool operator==(const KeyMods&) const = default;
};

enum class MouseButton : std::uint8_t { kLeft, kMiddle, kRight, kX1, kX2 };

/// Gamepad buttons by position, not label: kSouth is A on Xbox, Cross on PlayStation, B on
/// Nintendo layouts.
enum class GamepadButton : std::uint8_t {
  kSouth,
  kEast,
  kWest,
  kNorth,
  kBack,
  kGuide,
  kStart,
  kLeftStick,
  kRightStick,
  kLeftShoulder,
  kRightShoulder,
  kDpadUp,
  kDpadDown,
  kDpadLeft,
  kDpadRight,
};

enum class GamepadAxis : std::uint8_t {
  kLeftX,
  kLeftY,
  kRightX,
  kRightY,
  kLeftTrigger,
  kRightTrigger,
};

/// Identifies one connected gamepad for as long as it stays connected.
using GamepadId = std::uint32_t;

struct KeyEvent {
  Key key = Key::kEscape;
  bool pressed = false;
  /// True for the auto-repeat presses the OS sends while a key is held.
  bool repeat = false;
  KeyMods mods{};

  bool operator==(const KeyEvent&) const = default;
};

/// Text typed while text input is on (Platform::set_text_input), after the OS input method has
/// composed it. UTF-8.
struct TextInputEvent {
  std::string text{};

  bool operator==(const TextInputEvent&) const = default;
};

/// The pointer moved. Position in window pixels (see WindowMetrics); map it to the virtual
/// canvas with window_to_virtual().
struct PointerMoveEvent {
  PointF position{};

  bool operator==(const PointerMoveEvent&) const = default;
};

struct PointerButtonEvent {
  MouseButton button = MouseButton::kLeft;
  bool pressed = false;
  /// In window pixels.
  PointF position{};
  /// 1 for a single click, 2 for a double click, and so on.
  std::uint8_t clicks = 1;

  bool operator==(const PointerButtonEvent&) const = default;
};

/// Scroll amount in notches (fractional on touchpads). Positive dy means scroll up, positive dx
/// scroll right, with the user's OS "natural scrolling" preference already applied.
struct WheelEvent {
  float dx = 0;
  float dy = 0;

  bool operator==(const WheelEvent&) const = default;
};

struct GamepadConnectedEvent {
  GamepadId id = 0;

  bool operator==(const GamepadConnectedEvent&) const = default;
};

struct GamepadDisconnectedEvent {
  GamepadId id = 0;

  bool operator==(const GamepadDisconnectedEvent&) const = default;
};

struct GamepadButtonEvent {
  GamepadId id = 0;
  GamepadButton button = GamepadButton::kSouth;
  bool pressed = false;

  bool operator==(const GamepadButtonEvent&) const = default;
};

/// Stick axes go from -1 to 1 (negative is left/up); triggers from 0 (released) to 1.
struct GamepadAxisEvent {
  GamepadId id = 0;
  GamepadAxis axis = GamepadAxis::kLeftX;
  float value = 0;

  bool operator==(const GamepadAxisEvent&) const = default;
};

/// The window's pixel size changed (resize, or a move to a display with another density).
/// Recompute the viewport from the new size.
struct WindowResizedEvent {
  Size pixels{};

  bool operator==(const WindowResizedEvent&) const = default;
};

struct WindowFocusEvent {
  bool focused = false;

  bool operator==(const WindowFocusEvent&) const = default;
};

/// The user asked to quit (closed the window, Cmd+Q, or the OS is shutting the app down).
struct QuitRequestedEvent {
  bool operator==(const QuitRequestedEvent&) const = default;
};

/// Everything the platform reports to the engine.
using InputEvent =
    std::variant<KeyEvent, TextInputEvent, PointerMoveEvent, PointerButtonEvent, WheelEvent,
                 GamepadConnectedEvent, GamepadDisconnectedEvent, GamepadButtonEvent,
                 GamepadAxisEvent, WindowResizedEvent, WindowFocusEvent, QuitRequestedEvent>;

}  // namespace vnmaki
