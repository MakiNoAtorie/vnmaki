#pragma once

// Remappable input actions (docs/SPEC.md section 17): which keys and gamepad buttons do what in
// the story. The UI's own navigation (Tab, Esc, the arrows, the d-pad, Enter in a menu) is not
// part of the map, and the mouse buttons and the wheel stay as they are.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/platform/input.hpp"

namespace vnmaki {

enum class InputAction : std::uint8_t { kAdvance, kRollback, kAuto, kSkip, kBacklog, kMenu };

inline constexpr std::array kInputActions{InputAction::kAdvance, InputAction::kRollback,
                                          InputAction::kAuto,    InputAction::kSkip,
                                          InputAction::kBacklog, InputAction::kMenu};

/// The id of an action: "advance", "rollback", "auto", "skip", "backlog" or "menu". It is the
/// suffix of the preference `input_<id>`.
[[nodiscard]] std::string_view action_id(InputAction action);
/// What the Controls screen calls an action.
[[nodiscard]] std::string_view action_label(InputAction action);
/// The action with this id, or nothing.
[[nodiscard]] std::optional<InputAction> action_from_id(std::string_view id);

/// The keys and gamepad buttons of one action. Any of them triggers it.
struct Binding {
  std::vector<Key> keys{};
  std::vector<GamepadButton> buttons{};

  bool operator==(const Binding&) const = default;
};

/// "key:enter key:space pad:south": the form in the preferences (`prefs.input_<id>`). Names are
/// lower case with `_`: key:page_up, key:digit0, pad:left_shoulder.
[[nodiscard]] std::string format_binding(const Binding& binding);
/// Nothing when a name is unknown. An empty string is a binding with nothing in it.
[[nodiscard]] std::optional<Binding> parse_binding(std::string_view text);
/// For the Controls screen: "Enter, Space / South". "None" for an empty binding.
[[nodiscard]] std::string describe_binding(const Binding& binding);

/// Keys and buttons that the UI keeps for itself: they cannot be bound.
[[nodiscard]] bool is_reserved(Key key);
[[nodiscard]] bool is_reserved(GamepadButton button);

class InputMap {
 public:
  /// The default controls: see default_binding().
  InputMap();

  [[nodiscard]] static Binding default_binding(InputAction action);

  [[nodiscard]] const Binding& binding(InputAction action) const;
  void set_binding(InputAction action, Binding binding);
  /// Gives `action` this one key in place of its keys, and takes the key from the other actions.
  void bind_key(InputAction action, Key key);
  /// As bind_key, for a gamepad button.
  void bind_button(InputAction action, GamepadButton button);
  void reset();

  /// A key press (not a repeat) or a gamepad button press of `action`.
  [[nodiscard]] bool matches(InputAction action, const InputEvent& event) const;

  bool operator==(const InputMap&) const = default;

 private:
  std::array<Binding, kInputActions.size()> bindings_{};
};

}  // namespace vnmaki
