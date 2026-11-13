#include "engine/ui/screen_presenter.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

#include "engine/ui/ui_build.hpp"

namespace vnmaki {

ScreenPresenter::ScreenPresenter(ScriptRuntime& runtime, UiHost& host)
    : runtime_(&runtime), host_(&host) {
  sync();
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
    host_->show(std::string{kMenuScreenName}, make_menu_screen(menu->options, host_->canvas()),
                true);
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
    const auto* index = std::get_if<std::int64_t>(&value);
    if (index == nullptr) {
      return;
    }
    runtime_->choose(static_cast<std::size_t>(*index));
  } else {
    runtime_->return_screen(value);
  }
  sync();
}

bool ScreenPresenter::handle_event(const InputEvent& event, const Viewport& viewport) {
  const UiEventResult result = host_->handle_event(event, viewport);
  if (result.activation && shown_ && result.activation->screen == shown_->first) {
    answer(result.activation->value);
  }
  return result.consumed;
}

}  // namespace vnmaki
