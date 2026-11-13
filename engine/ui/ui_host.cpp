#include "engine/ui/ui_host.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "engine/ui/ui_build.hpp"
#include "engine/ui/ui_focus.hpp"

namespace vnmaki {

UiHost::UiHost(Size canvas) : canvas_(canvas), measure_(estimate_text_size) {}

void UiHost::set_text_measure(TextMeasure measure) {
  measure_ = std::move(measure);
  dirty_ = true;
}

std::string UiHost::check_screen(std::string_view name, const ScreenNodeDef& screen) {
  try {
    static_cast<void>(build_ui_tree(screen));
  } catch (const UiBuildError& error) {
    return "screen '" + std::string{name} + "': " + error.what();
  }
  return {};
}

std::string UiHost::show_screen(std::string_view name, const ScreenNodeDef& screen) {
  std::string problem = check_screen(name, screen);
  if (problem.empty()) {
    UiNode root = build_ui_tree(screen);
    const bool modal = root.modal;
    show(std::string{name}, std::move(root), modal);
  }
  return problem;
}

void UiHost::hide_screen(std::string_view name) { hide(name); }

void UiHost::show(std::string name, UiNode root, bool modal) {
  hide(name);
  if (modal) {
    static_cast<void>(move_focus(root, FocusMove::kNext));
  }
  screens_.push_back({.name = std::move(name), .root = std::move(root), .modal = modal});
  dirty_ = true;
  sync_text_input();
}

bool UiHost::hide(std::string_view name) {
  const auto found = std::ranges::find(screens_, name, &Screen::name);
  if (found == screens_.end()) {
    return false;
  }
  screens_.erase(found);
  sync_text_input();
  return true;
}

bool UiHost::is_shown(std::string_view name) const {
  return std::ranges::find(screens_, name, &Screen::name) != screens_.end();
}

bool UiHost::has_modal() const { return std::ranges::any_of(screens_, &Screen::modal); }

bool UiHost::is_top_modal(std::string_view name) const {
  const auto top = std::ranges::find_if(screens_.rbegin(), screens_.rend(), &Screen::modal);
  return top != screens_.rend() && top->name == name;
}

UiNode* UiHost::find(std::string_view name) {
  refresh_bindings();
  ensure_layout();
  const auto found = std::ranges::find(screens_, name, &Screen::name);
  return found == screens_.end() ? nullptr : &found->root;
}

std::vector<const UiNode*> UiHost::roots() {
  refresh_bindings();
  ensure_layout();
  std::vector<const UiNode*> trees;
  trees.reserve(screens_.size());
  for (const Screen& screen : screens_) {
    trees.push_back(&screen.root);
  }
  return trees;
}

void UiHost::ensure_layout() {
  if (!dirty_) {
    return;
  }
  for (Screen& screen : screens_) {
    layout_tree(screen.root, canvas_, measure_);
  }
  dirty_ = false;
}

namespace {

std::optional<double> as_number(const ScreenValue& value) {
  if (const auto* integer = std::get_if<std::int64_t>(&value)) {
    return static_cast<double>(*integer);
  }
  if (const auto* number = std::get_if<double>(&value)) {
    return *number;
  }
  return std::nullopt;
}

// Sets `relayout` when the text of an input changed, which can change the size of the node.
void refresh_node(UiNode& node, const ScriptRuntime& runtime, bool& relayout) {
  if (node.bind) {
    const ScreenValue value = runtime.binding(node.bind->table, node.bind->key);
    if (node.kind == NodeKind::kInput) {
      const auto* text = std::get_if<std::string>(&value);
      const std::string shown = text != nullptr ? *text : std::string{};
      relayout = relayout || shown != node.text;
      node.text = shown;
    } else if (node.kind == NodeKind::kToggle) {
      const auto* flag = std::get_if<bool>(&value);
      node.checked = flag != nullptr && *flag;
    } else if (node.kind == NodeKind::kSlider || node.kind == NodeKind::kBar) {
      node.value = std::clamp(as_number(value).value_or(node.min), node.min, node.max);
    }
  }
  for (UiNode& child : node.children) {
    refresh_node(child, runtime, relayout);
  }
}

constexpr unsigned char kContinuationMask = 0xC0;
constexpr unsigned char kContinuation = 0x80;
constexpr unsigned char kFirstPrintable = 0x20;
constexpr unsigned char kDeleteCharacter = 0x7F;

bool is_continuation(char byte) {
  return (static_cast<unsigned char>(byte) & kContinuationMask) == kContinuation;
}

std::size_t code_points(std::string_view text) {
  return static_cast<std::size_t>(
      std::ranges::count_if(text, [](char byte) { return !is_continuation(byte); }));
}

// `text` with `typed` added at the end: whole characters, no control characters, and no more than
// `max_length` characters in all.
std::string with_typed(std::string text, std::string_view typed, std::size_t max_length) {
  std::size_t count = code_points(text);
  std::size_t first = 0;
  while (first < typed.size() && count < max_length) {
    std::size_t end = first + 1;
    while (end < typed.size() && is_continuation(typed[end])) {
      ++end;
    }
    const auto lead = static_cast<unsigned char>(typed[first]);
    if (lead >= kFirstPrintable && lead != kDeleteCharacter && !is_continuation(typed[first])) {
      text.append(typed.substr(first, end - first));
      ++count;
    }
    first = end;
  }
  return text;
}

std::string without_last_character(std::string text) {
  while (!text.empty() && is_continuation(text.back())) {
    text.pop_back();
  }
  if (!text.empty()) {
    text.pop_back();
  }
  return text;
}

bool is_navigation_key(Key key) {
  return key == Key::kTab || key == Key::kEscape || key == Key::kUp || key == Key::kDown ||
         key == Key::kLeft || key == Key::kRight;
}

// How much a key press moves a slider.
double slider_step(const UiNode& node) {
  constexpr double kSteps = 20;
  return node.step > 0 ? node.step : (node.max - node.min) / kSteps;
}

enum class Intent : std::uint8_t { kNone, kMove, kActivate, kCancel, kTake };

struct KeyIntent {
  Intent intent = Intent::kNone;
  FocusMove move = FocusMove::kNext;
  /// Tab and the west button also give the keyboard to a non-modal screen.
  bool takes_keyboard = false;
};

std::optional<FocusMove> direction_of_key(const KeyEvent& key) {
  switch (key.key) {
    case Key::kTab:
      return key.mods.shift ? FocusMove::kPrevious : FocusMove::kNext;
    case Key::kUp:
      return FocusMove::kUp;
    case Key::kDown:
      return FocusMove::kDown;
    case Key::kLeft:
      return FocusMove::kLeft;
    case Key::kRight:
      return FocusMove::kRight;
    default:
      return std::nullopt;
  }
}

KeyIntent key_intent(const KeyEvent& key) {
  if (const std::optional<FocusMove> move = direction_of_key(key)) {
    return {.intent = Intent::kMove, .move = *move, .takes_keyboard = key.key == Key::kTab};
  }
  if (key.key == Key::kEnter || key.key == Key::kKeypadEnter || key.key == Key::kSpace) {
    return {.intent = key.repeat ? Intent::kNone : Intent::kActivate};
  }
  if (key.key == Key::kEscape) {
    return {.intent = Intent::kCancel};
  }
  return {};
}

KeyIntent pad_intent(const GamepadButtonEvent& pad) {
  switch (pad.button) {
    case GamepadButton::kDpadUp:
      return {.intent = Intent::kMove, .move = FocusMove::kUp};
    case GamepadButton::kDpadDown:
      return {.intent = Intent::kMove, .move = FocusMove::kDown};
    case GamepadButton::kDpadLeft:
      return {.intent = Intent::kMove, .move = FocusMove::kLeft};
    case GamepadButton::kDpadRight:
      return {.intent = Intent::kMove, .move = FocusMove::kRight};
    case GamepadButton::kSouth:
      return {.intent = Intent::kActivate};
    case GamepadButton::kEast:
      return {.intent = Intent::kCancel};
    case GamepadButton::kWest:
      return {.intent = Intent::kTake, .takes_keyboard = true};
    default:
      return {};
  }
}

}  // namespace

void UiHost::refresh_bindings() {
  if (bindings_ == nullptr) {
    return;
  }
  for (Screen& screen : screens_) {
    refresh_node(screen.root, *bindings_, dirty_);
  }
}

void UiHost::set_platform(Platform* platform) {
  platform_ = platform;
  sync_text_input();
}

UiNode* UiHost::focused_input() {
  Screen* screen = keyboard_screen();
  if (screen == nullptr) {
    return nullptr;
  }
  UiNode* focused = focused_node(screen->root);
  return focused != nullptr && focused->kind == NodeKind::kInput ? focused : nullptr;
}

bool UiHost::wants_text_input() { return focused_input() != nullptr; }

void UiHost::sync_text_input() {
  const bool wanted = wants_text_input();
  if (platform_ != nullptr && wanted != text_input_on_) {
    text_input_on_ = wanted;
    platform_->set_text_input(wanted);
  }
}

UiEventResult UiHost::text_event(const TextInputEvent& typed) {
  UiNode* input = focused_input();
  if (input == nullptr) {
    return {};
  }
  std::string text = with_typed(input->text, typed.text, input->max_length);
  if (text != input->text) {
    write_value(*input, std::move(text));
  }
  return {.consumed = true};
}

// Changes what a bound node follows; the node keeps the value if nothing is bound.
void UiHost::write_value(UiNode& node, ScreenValue value) {
  if (node.bind && bindings_ != nullptr) {
    static_cast<void>(bindings_->set_binding(node.bind->table, node.bind->key, std::move(value)));
    refresh_node(node, *bindings_, dirty_);
  } else if (const auto* flag = std::get_if<bool>(&value)) {
    node.checked = *flag;
  } else if (const auto* text = std::get_if<std::string>(&value)) {
    node.text = *text;
    dirty_ = true;
  } else if (const auto number = as_number(value)) {
    node.value = *number;
  }
}

void UiHost::set_slider(UiNode& node, double value) {
  if (node.step > 0) {
    value = node.min + (std::round((value - node.min) / node.step) * node.step);
  }
  write_value(node, std::clamp(value, node.min, node.max));
}

void UiHost::drag_slider(PointF point) {
  if (!drag_) {
    return;
  }
  const auto screen = std::ranges::find(screens_, drag_->first, &Screen::name);
  if (screen == screens_.end()) {
    drag_.reset();
    return;
  }
  const std::vector<UiNode*> nodes = focusable_nodes(screen->root);
  if (drag_->second >= nodes.size() || nodes[drag_->second]->kind != NodeKind::kSlider) {
    drag_.reset();
    return;
  }
  UiNode& slider = *nodes[drag_->second];
  const float share = std::clamp((point.x - slider.rect.x) / slider.rect.width, 0.0F, 1.0F);
  set_slider(slider, slider.min + (static_cast<double>(share) * (slider.max - slider.min)));
}

UiHost::Screen* UiHost::top_modal() {
  const auto found = std::ranges::find_if(screens_.rbegin(), screens_.rend(), &Screen::modal);
  return found == screens_.rend() ? nullptr : &*found;
}

UiHost::Screen* UiHost::keyboard_screen() {
  if (Screen* modal = top_modal()) {
    return modal;
  }
  const auto found = std::ranges::find(screens_, captured_, &Screen::name);
  return found == screens_.end() ? nullptr : &*found;
}

UiEventResult UiHost::pointer_event(PointF point, bool click) {
  ensure_layout();
  bool shadowed = false;  // a modal screen hides the ones below it from the pointer
  UiNode* hit = nullptr;
  Screen* hit_screen = nullptr;
  const Screen* typing = focused_input() != nullptr ? keyboard_screen() : nullptr;
  for (auto screen = screens_.rbegin(); screen != screens_.rend(); ++screen) {
    clear_hover(screen->root);
    if (shadowed || hit != nullptr) {
      continue;
    }
    UiNode* node = focusable_at(screen->root, point);
    if (node != nullptr) {
      node->state.hovered = true;
      if (click || &*screen != typing) {  // moving the pointer leaves an input its focus
        static_cast<void>(set_focus(screen->root, node));
      }
      hit = node;
      hit_screen = &*screen;
    }
    shadowed = screen->modal;
  }
  UiEventResult result;
  if (!click) {
    return result;
  }
  result.consumed = hit != nullptr || shadowed;
  if (hit == nullptr) {
    return result;
  }
  if (hit->kind == NodeKind::kToggle) {
    write_value(*hit, !hit->checked);
  } else if (hit->kind == NodeKind::kInput) {
    if (!hit_screen->modal) {
      captured_ = hit_screen->name;  // the typed text must reach the screen
    }
  } else if (hit->kind == NodeKind::kSlider) {
    const std::vector<UiNode*> nodes = focusable_nodes(hit_screen->root);
    const auto index = static_cast<std::size_t>(std::ranges::find(nodes, hit) - nodes.begin());
    drag_ = {hit_screen->name, index};
    drag_slider(point);
  } else if (hit->returns) {
    result.activation = ScreenActivation{.screen = hit_screen->name, .value = *hit->returns};
  }
  return result;
}

UiEventResult UiHost::activate_focused(Screen& screen) {
  UiEventResult result{.consumed = true};
  UiNode* focused = focused_node(screen.root);
  if (focused == nullptr) {
    return result;
  }
  if (focused->kind == NodeKind::kToggle) {
    write_value(*focused, !focused->checked);
  } else if (focused->returns) {
    result.activation = ScreenActivation{.screen = screen.name, .value = *focused->returns};
  }
  return result;
}

UiEventResult UiHost::key_event(const InputEvent& event) {
  if (const auto* key = std::get_if<KeyEvent>(&event)) {
    if (UiNode* input = focused_input()) {
      if (key->pressed && key->key == Key::kBackspace) {
        write_value(*input, without_last_character(input->text));
        return {.consumed = true};
      }
      if (key->pressed && !key->repeat &&
          (key->key == Key::kEnter || key->key == Key::kKeypadEnter) && input->returns) {
        return {.consumed = true,
                .activation =
                    ScreenActivation{.screen = keyboard_screen()->name, .value = *input->returns}};
      }
      if (!is_navigation_key(key->key)) {
        return {.consumed = true};  // typing: a key is not the story's (A, S, L, Space, ...)
      }
    }
  }
  KeyIntent intent;
  if (const auto* key = std::get_if<KeyEvent>(&event); key != nullptr && key->pressed) {
    intent = key_intent(*key);
  } else if (const auto* pad = std::get_if<GamepadButtonEvent>(&event);
             pad != nullptr && pad->pressed) {
    intent = pad_intent(*pad);
  }
  if (intent.intent == Intent::kNone) {
    return {};
  }
  Screen* screen = keyboard_screen();
  if (screen == nullptr) {
    // No screen has the keyboard: Tab or west gives it to the top screen that has a button.
    ensure_layout();
    for (auto candidate = screens_.rbegin(); intent.takes_keyboard && candidate != screens_.rend();
         ++candidate) {
      if (!focusable_nodes(candidate->root).empty()) {
        captured_ = candidate->name;
        static_cast<void>(move_focus(candidate->root, FocusMove::kNext));
        return {.consumed = true};
      }
    }
    return {};
  }
  ensure_layout();
  UiNode* focused = focused_node(screen->root);
  switch (intent.intent) {
    case Intent::kActivate:
      return activate_focused(*screen);
    case Intent::kCancel:
      if (!screen->modal) {  // the player gives the keys back
        captured_.clear();
        static_cast<void>(set_focus(screen->root, nullptr));
        return {.consumed = true};
      }
      if (screen->root.cancel) {
        return {
            .consumed = true,
            .activation = ScreenActivation{.screen = screen->name, .value = *screen->root.cancel}};
      }
      return {};
    case Intent::kMove: {
      const bool sideways = intent.move == FocusMove::kLeft || intent.move == FocusMove::kRight;
      if (sideways && focused != nullptr && focused->kind == NodeKind::kSlider) {
        const double direction = intent.move == FocusMove::kLeft ? -1 : 1;
        set_slider(*focused, focused->value + (direction * slider_step(*focused)));
      } else {
        static_cast<void>(move_focus(screen->root, intent.move));
      }
      return {.consumed = true};
    }
    case Intent::kTake:  // the keyboard is taken already
    case Intent::kNone:
      break;
  }
  return {};
}

UiEventResult UiHost::handle_event(const InputEvent& event, const Viewport& viewport) {
  UiEventResult result = dispatch_event(event, viewport);
  // A modal screen takes every player input, so none reaches the story (rollback, auto, skip).
  if (!std::holds_alternative<GamepadConnectedEvent>(event) &&
      !std::holds_alternative<GamepadDisconnectedEvent>(event) &&
      !std::holds_alternative<WindowResizedEvent>(event) &&
      !std::holds_alternative<WindowFocusEvent>(event) &&
      !std::holds_alternative<QuitRequestedEvent>(event) && has_modal()) {
    result.consumed = true;
  }
  sync_text_input();
  return result;
}

UiEventResult UiHost::dispatch_event(const InputEvent& event, const Viewport& viewport) {
  refresh_bindings();
  if (const auto* typed = std::get_if<TextInputEvent>(&event)) {
    return text_event(*typed);
  }
  if (const auto* move = std::get_if<PointerMoveEvent>(&event)) {
    const PointF point = window_to_virtual(viewport, move->position);
    if (drag_) {
      ensure_layout();
      drag_slider(point);
      return {.consumed = true};
    }
    return pointer_event(point, false);
  }
  if (const auto* button = std::get_if<PointerButtonEvent>(&event)) {
    if (button->button != MouseButton::kLeft) {
      return {};
    }
    if (button->pressed) {
      return pointer_event(window_to_virtual(viewport, button->position), true);
    }
    return {.consumed = std::exchange(drag_, std::nullopt).has_value()};
  }
  return key_event(event);
}

}  // namespace vnmaki
