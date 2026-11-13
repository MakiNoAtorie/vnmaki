#include "engine/ui/screen_presenter.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "engine/ui/ui_build.hpp"

namespace vnmaki {

ScreenPresenter::ScreenPresenter(ScriptRuntime& runtime, UiHost& host)
    : runtime_(&runtime), host_(&host) {
  sync();
}

// The Lua screen `choice` for the options, or the built-in one if the game's screen fails (a
// broken screen must not stop the story).
UiNode ScreenPresenter::menu_tree(const std::vector<std::string>& options) {
  ScreenNodeDef args{.kind = "args"};
  args.children.reserve(options.size());
  for (const std::string& option : options) {
    args.children.push_back({.kind = "option", .props = {{"text", option}}});
  }
  ScreenNodeDef tree;
  if (runtime_->build_screen("choice", args, tree).empty()) {
    try {
      return build_ui_tree(tree);
    } catch (const UiBuildError&) {
      // falls through to the built-in menu
    }
  }
  return make_menu_screen(options, host_->canvas());
}

void ScreenPresenter::sync() {
  const BlockingRequest* request =
      runtime_->state() == RunState::kWaiting ? runtime_->request() : nullptr;
  const auto* menu = request != nullptr ? std::get_if<MenuRequest>(request) : nullptr;
  const auto* call = request != nullptr ? std::get_if<CallScreenRequest>(request) : nullptr;
  const ScriptPosition position = runtime_->position();
  if (shown_ && ((menu == nullptr && call == nullptr) || shown_->second != position)) {
    host_->hide(shown_->first);
    shown_.reset();
  }
  if (shown_) {
    return;
  }
  if (menu != nullptr) {
    host_->show(std::string{kMenuScreenName}, menu_tree(menu->options), true);
    shown_ = {std::string{kMenuScreenName}, position};
  } else if (call != nullptr) {
    host_->show(call->name, build_ui_tree(call->screen), true);
    shown_ = {call->name, position};
  }
}

void ScreenPresenter::answer(const ScreenValue& value) {
  const BlockingRequest* request = runtime_->request();
  if (request == nullptr) {
    return;
  }
  if (std::holds_alternative<MenuRequest>(*request)) {
    const auto* number = std::get_if<std::int64_t>(&value);
    const auto& options = std::get<MenuRequest>(*request).options;
    if (number == nullptr || *number < 1 || *number > static_cast<std::int64_t>(options.size())) {
      return;
    }
    runtime_->choose(static_cast<std::size_t>(*number - 1));
  } else {
    runtime_->return_screen(value);
  }
  sync();
}

bool ScreenPresenter::handle_event(const InputEvent& event, const Viewport& viewport) {
  const UiEventResult result = host_->handle_event(event, viewport);
  if (result.activation) {
    if (actions_ && actions_(*result.activation)) {
      sync();  // a load moved the story
    } else if (shown_ && result.activation->screen == shown_->first) {
      answer(result.activation->value);
    }
  }
  return result.consumed;
}

}  // namespace vnmaki
