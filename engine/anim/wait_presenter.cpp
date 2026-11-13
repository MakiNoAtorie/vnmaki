#include "engine/anim/wait_presenter.hpp"

#include <variant>

#include "engine/runtime/script_runtime.hpp"

namespace vnmaki {

namespace {

// The wait the story is at, or null.
const WaitRequest* current_wait(const ScriptRuntime& runtime) {
  const BlockingRequest* request =
      runtime.state() == RunState::kWaiting ? runtime.request() : nullptr;
  return request != nullptr ? std::get_if<WaitRequest>(request) : nullptr;
}

}  // namespace

WaitPresenter::WaitPresenter(ScriptRuntime& runtime, SceneCommands& scene)
    : runtime_(&runtime), scene_(&scene) {}

bool WaitPresenter::waiting() const { return current_wait(*runtime_) != nullptr; }

bool WaitPresenter::update(double seconds) {
  const WaitRequest* wait = current_wait(*runtime_);
  if (wait == nullptr) {
    pause_elapsed_ = 0;
    return false;
  }
  if (wait->tag == kPauseTag) {
    pause_elapsed_ += seconds;
    if (pause_elapsed_ < wait->seconds) {
      return false;
    }
  } else if (scene_->tweening(wait->tag)) {
    return false;
  }
  pause_elapsed_ = 0;
  runtime_->advance();
  return true;
}

bool WaitPresenter::skip() {
  const WaitRequest* wait = current_wait(*runtime_);
  if (wait == nullptr) {
    return false;
  }
  pause_elapsed_ = 0;
  scene_->finish_tweens(wait->tag);
  runtime_->advance();
  return true;
}

}  // namespace vnmaki
