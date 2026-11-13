#include "engine/ui/ui_host.hpp"

#include <algorithm>
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
    show(std::string{name}, build_ui_tree(screen), false);
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
}

bool UiHost::hide(std::string_view name) {
  const auto found = std::ranges::find(screens_, name, &Screen::name);
  if (found == screens_.end()) {
    return false;
  }
  screens_.erase(found);
  return true;
}

bool UiHost::is_shown(std::string_view name) const {
  return std::ranges::find(screens_, name, &Screen::name) != screens_.end();
}

bool UiHost::has_modal() const {
  return std::ranges::any_of(screens_, &Screen::modal);
}

UiNode* UiHost::find(std::string_view name) {
  ensure_layout();
  const auto found = std::ranges::find(screens_, name, &Screen::name);
  return found == screens_.end() ? nullptr : &found->root;
}

std::vector<const UiNode*> UiHost::roots() {
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

UiHost::Screen* UiHost::top_modal() {
  const auto found = std::ranges::find_if(screens_.rbegin(), screens_.rend(), &Screen::modal);
  return found == screens_.rend() ? nullptr : &*found;
}

UiEventResult UiHost::pointer_event(PointF point, bool click) {
  ensure_layout();
  bool shadowed = false;  // a modal screen hides the ones below it from the pointer
  const UiNode* hit = nullptr;
  const Screen* hit_screen = nullptr;
  for (auto screen = screens_.rbegin(); screen != screens_.rend(); ++screen) {
    clear_hover(screen->root);
    if (shadowed || hit != nullptr) {
      continue;
    }
    UiNode* node = focusable_at(screen->root, point);
    if (node != nullptr) {
      node->state.hovered = true;
      static_cast<void>(set_focus(screen->root, node));
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
  if (hit != nullptr && hit->returns) {
    result.activation = ScreenActivation{.screen = hit_screen->name, .value = *hit->returns};
  }
  return result;
}

UiEventResult UiHost::activate_focused() {
  UiEventResult result{.consumed = true};
  Screen* screen = top_modal();
  const UiNode* focused = screen != nullptr ? focused_node(screen->root) : nullptr;
  if (focused != nullptr && focused->returns) {
    result.activation = ScreenActivation{.screen = screen->name, .value = *focused->returns};
  }
  return result;
}

UiEventResult UiHost::handle_event(const InputEvent& event, const Viewport& viewport) {
  if (const auto* move = std::get_if<PointerMoveEvent>(&event)) {
    return pointer_event(window_to_virtual(viewport, move->position), false);
  }
  if (const auto* button = std::get_if<PointerButtonEvent>(&event)) {
    if (button->pressed && button->button == MouseButton::kLeft) {
      return pointer_event(window_to_virtual(viewport, button->position), true);
    }
    return {};
  }
  Screen* screen = top_modal();
  if (screen == nullptr) {
    return {};
  }
  ensure_layout();
  std::optional<FocusMove> move;
  bool activate = false;
  if (const auto* key = std::get_if<KeyEvent>(&event); key != nullptr && key->pressed) {
    switch (key->key) {
      case Key::kTab:
        move = key->mods.shift ? FocusMove::kPrevious : FocusMove::kNext;
        break;
      case Key::kUp:
        move = FocusMove::kUp;
        break;
      case Key::kDown:
        move = FocusMove::kDown;
        break;
      case Key::kLeft:
        move = FocusMove::kLeft;
        break;
      case Key::kRight:
        move = FocusMove::kRight;
        break;
      case Key::kEnter:
      case Key::kKeypadEnter:
      case Key::kSpace:
        activate = !key->repeat;
        break;
      default:
        return {};
    }
    if (activate) {
      return activate_focused();
    }
  } else if (const auto* pad = std::get_if<GamepadButtonEvent>(&event);
             pad != nullptr && pad->pressed) {
    switch (pad->button) {
      case GamepadButton::kDpadUp:
        move = FocusMove::kUp;
        break;
      case GamepadButton::kDpadDown:
        move = FocusMove::kDown;
        break;
      case GamepadButton::kDpadLeft:
        move = FocusMove::kLeft;
        break;
      case GamepadButton::kDpadRight:
        move = FocusMove::kRight;
        break;
      case GamepadButton::kSouth:
        return activate_focused();
      default:
        return {};
    }
  }
  if (!move) {
    return {};
  }
  static_cast<void>(move_focus(screen->root, *move));
  return {.consumed = true};
}

}  // namespace vnmaki
