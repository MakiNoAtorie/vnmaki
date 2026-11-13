#include "engine/platform/input_map.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace vnmaki {

namespace {

constexpr std::string_view kKeyPrefix = "key:";
constexpr std::string_view kPadPrefix = "pad:";

constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::kRightGui) + 1;
constexpr std::size_t kButtonCount = static_cast<std::size_t>(GamepadButton::kDpadRight) + 1;

// In the order of the enumerators.
constexpr std::array<std::string_view, kKeyCount> kKeyNames{
    "a",          "b",           "c",         "d",          "e",          "f",
    "g",          "h",           "i",         "j",          "k",          "l",
    "m",          "n",           "o",         "p",          "q",          "r",
    "s",          "t",           "u",         "v",          "w",          "x",
    "y",          "z",           "digit0",    "digit1",     "digit2",     "digit3",
    "digit4",     "digit5",      "digit6",    "digit7",     "digit8",     "digit9",
    "f1",         "f2",          "f3",        "f4",         "f5",         "f6",
    "f7",         "f8",          "f9",        "f10",        "f11",        "f12",
    "escape",     "enter",       "keypad_enter", "space",   "tab",        "backspace",
    "delete",     "insert",      "home",      "end",        "page_up",    "page_down",
    "left",       "right",       "up",        "down",       "left_shift", "right_shift",
    "left_ctrl",  "right_ctrl",  "left_alt",  "right_alt",  "left_gui",   "right_gui"};

constexpr std::array<std::string_view, kButtonCount> kButtonNames{
    "south",      "east",        "west",       "north",         "back",
    "guide",      "start",       "left_stick", "right_stick",   "left_shoulder",
    "right_shoulder", "dpad_up", "dpad_down",  "dpad_left",     "dpad_right"};

std::string_view key_name(Key key) { return kKeyNames[static_cast<std::size_t>(key)]; }
std::string_view button_name(GamepadButton button) {
  return kButtonNames[static_cast<std::size_t>(button)];
}

// "page_up" becomes "Page up".
std::string display_name(std::string_view id) {
  std::string text{id};
  std::ranges::replace(text, '_', ' ');
  if (!text.empty()) {
    text.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(text.front())));
  }
  return text;
}

template <typename T, std::size_t N>
std::optional<T> find_name(const std::array<std::string_view, N>& names, std::string_view name) {
  const auto found = std::ranges::find(names, name);
  if (found == names.end()) {
    return std::nullopt;
  }
  return static_cast<T>(found - names.begin());
}

template <typename T>
std::string join_names(const std::vector<T>& values, std::string (*name)(T)) {
  std::string text;
  for (const T value : values) {
    text += (text.empty() ? "" : ", ") + name(value);
  }
  return text;
}

}  // namespace

std::string_view action_id(InputAction action) {
  switch (action) {
    case InputAction::kAdvance:
      return "advance";
    case InputAction::kRollback:
      return "rollback";
    case InputAction::kAuto:
      return "auto";
    case InputAction::kSkip:
      return "skip";
    case InputAction::kBacklog:
      return "backlog";
    case InputAction::kMenu:
      return "menu";
  }
  return {};
}

std::string_view action_label(InputAction action) {
  switch (action) {
    case InputAction::kAdvance:
      return "Advance text";
    case InputAction::kRollback:
      return "Roll back";
    case InputAction::kAuto:
      return "Auto-advance";
    case InputAction::kSkip:
      return "Skip";
    case InputAction::kBacklog:
      return "Backlog";
    case InputAction::kMenu:
      return "Preferences";
  }
  return {};
}

std::optional<InputAction> action_from_id(std::string_view id) {
  for (const InputAction action : kInputActions) {
    if (action_id(action) == id) {
      return action;
    }
  }
  return std::nullopt;
}

std::string format_binding(const Binding& binding) {
  std::string text;
  const auto add = [&](std::string_view prefix, std::string_view name) {
    if (!text.empty()) {
      text += ' ';
    }
    text += prefix;
    text += name;
  };
  for (const Key key : binding.keys) {
    add(kKeyPrefix, key_name(key));
  }
  for (const GamepadButton button : binding.buttons) {
    add(kPadPrefix, button_name(button));
  }
  return text;
}

std::optional<Binding> parse_binding(std::string_view text) {
  Binding binding;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = std::min(text.find(' ', start), text.size());
    const std::string_view token = text.substr(start, end - start);
    start = end + 1;
    if (token.empty()) {
      continue;
    }
    if (token.starts_with(kKeyPrefix)) {
      const auto key = find_name<Key>(kKeyNames, token.substr(kKeyPrefix.size()));
      if (!key) {
        return std::nullopt;
      }
      binding.keys.push_back(*key);
    } else if (token.starts_with(kPadPrefix)) {
      const auto button = find_name<GamepadButton>(kButtonNames, token.substr(kPadPrefix.size()));
      if (!button) {
        return std::nullopt;
      }
      binding.buttons.push_back(*button);
    } else {
      return std::nullopt;
    }
  }
  return binding;
}

std::string describe_binding(const Binding& binding) {
  const std::string keys =
      join_names<Key>(binding.keys, [](Key key) { return display_name(key_name(key)); });
  const std::string buttons = join_names<GamepadButton>(
      binding.buttons, [](GamepadButton button) { return display_name(button_name(button)); });
  if (keys.empty() && buttons.empty()) {
    return "None";
  }
  if (keys.empty() || buttons.empty()) {
    return keys + buttons;
  }
  return keys + " / " + buttons;
}

bool is_reserved(Key key) {
  switch (key) {
    case Key::kEscape:
    case Key::kTab:
    case Key::kLeft:
    case Key::kRight:
    case Key::kUp:
    case Key::kDown:
      return true;
    default:
      return false;
  }
}

bool is_reserved(GamepadButton button) {
  switch (button) {
    case GamepadButton::kEast:
    case GamepadButton::kWest:
    case GamepadButton::kGuide:
    case GamepadButton::kDpadUp:
    case GamepadButton::kDpadDown:
    case GamepadButton::kDpadLeft:
    case GamepadButton::kDpadRight:
      return true;
    default:
      return false;
  }
}

InputMap::InputMap() {
  for (const InputAction action : kInputActions) {
    bindings_[static_cast<std::size_t>(action)] = default_binding(action);
  }
}

Binding InputMap::default_binding(InputAction action) {
  switch (action) {
    case InputAction::kAdvance:
      return {.keys = {Key::kEnter, Key::kKeypadEnter, Key::kSpace},
              .buttons = {GamepadButton::kSouth}};
    case InputAction::kRollback:
      return {.keys = {Key::kPageUp}, .buttons = {GamepadButton::kLeftShoulder}};
    case InputAction::kAuto:
      return {.keys = {Key::kA}, .buttons = {}};
    case InputAction::kSkip:
      return {.keys = {Key::kS}, .buttons = {GamepadButton::kRightShoulder}};
    case InputAction::kBacklog:
      return {.keys = {Key::kL}, .buttons = {GamepadButton::kNorth}};
    case InputAction::kMenu:
      return {.keys = {Key::kEscape}, .buttons = {GamepadButton::kStart}};
  }
  return {};
}

const Binding& InputMap::binding(InputAction action) const {
  return bindings_[static_cast<std::size_t>(action)];
}

void InputMap::set_binding(InputAction action, Binding binding) {
  bindings_[static_cast<std::size_t>(action)] = std::move(binding);
}

void InputMap::bind_key(InputAction action, Key key) {
  for (Binding& other : bindings_) {
    std::erase(other.keys, key);
  }
  bindings_[static_cast<std::size_t>(action)].keys = {key};
}

void InputMap::bind_button(InputAction action, GamepadButton button) {
  for (Binding& other : bindings_) {
    std::erase(other.buttons, button);
  }
  bindings_[static_cast<std::size_t>(action)].buttons = {button};
}

void InputMap::reset() { *this = InputMap{}; }

bool InputMap::matches(InputAction action, const InputEvent& event) const {
  const Binding& wanted = binding(action);
  if (const auto* key = std::get_if<KeyEvent>(&event)) {
    return key->pressed && !key->repeat &&
           std::ranges::find(wanted.keys, key->key) != wanted.keys.end();
  }
  if (const auto* pad = std::get_if<GamepadButtonEvent>(&event)) {
    return pad->pressed && std::ranges::find(wanted.buttons, pad->button) != wanted.buttons.end();
  }
  return false;
}

}  // namespace vnmaki
