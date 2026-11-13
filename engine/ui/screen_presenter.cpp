#include "engine/ui/screen_presenter.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "engine/dialogue/rollback_input.hpp"
#include "engine/ui/ui_build.hpp"

namespace vnmaki {

ScreenPresenter::ScreenPresenter(ScriptRuntime& runtime, UiHost& host)
    : runtime_(&runtime), host_(&host) {
  sync();
}

// The Lua screen `choice` for the options, or the built-in one if the game's screen fails (a
// broken screen must not stop the story).
UiNode ScreenPresenter::menu_tree(const MenuRequest& menu) {
  const std::vector<std::string>& options = menu.options;
  // In NVL mode the options show inside the NVL screen, under the page.
  ScreenNodeDef args;
  if (menu.nvl) {
    args = make_nvl_args(*menu.nvl, false, options);
  } else {
    args = {.kind = "args"};
    args.children.reserve(options.size());
    for (const std::string& option : options) {
      args.children.push_back({.kind = "option", .props = {{"text", option}}});
    }
  }
  ScreenNodeDef tree;
  if (runtime_->build_screen(menu.nvl ? "nvl" : "choice", args, tree).empty()) {
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
  // A replay to the same position (a quick-load) can bring other options or another screen.
  if (shown_ && ((menu == nullptr && call == nullptr) || shown_->position != position ||
                 shown_->request != *request)) {
    host_->hide(shown_->name);
    shown_.reset();
  }
  if (shown_) {
    return;
  }
  if (menu != nullptr) {
    host_->show(std::string{kMenuScreenName}, menu_tree(*menu), true);
    shown_ = Shown{std::string{kMenuScreenName}, position, *request};
  } else if (call != nullptr) {
    host_->show(call->name, build_ui_tree(call->screen), true);
    shown_ = Shown{call->name, position, *request};
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
  // The player may roll back from a menu or `call_screen`, but not from a screen over it.
  if (shown_ && is_rollback_event(event) && host_->is_top_modal(shown_->name) &&
      !host_->wants_text_input()) {
    return false;
  }
  const UiEventResult result = host_->handle_event(event, viewport);
  if (result.activation) {
    if (actions_ && actions_(*result.activation)) {
      sync();  // a load moved the story
    } else if (shown_ && result.activation->screen == shown_->name) {
      answer(result.activation->value);
    }
  }
  return result.consumed;
}

}  // namespace vnmaki
