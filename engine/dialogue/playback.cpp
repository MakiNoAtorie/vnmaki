#include "engine/dialogue/playback.hpp"

#include <utility>

#include "engine/dialogue/dialogue_presenter.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/saves/persistent_store.hpp"

namespace vnmaki {

Playback::Playback(ScriptRuntime& runtime, DialoguePresenter& presenter,
                   PersistentStore& persistent, PlaybackOptions options)
    : runtime_(&runtime),
      presenter_(&presenter),
      persistent_(&persistent),
      options_(std::move(options)) {}

void Playback::set_auto(bool on) {
  auto_ = on;
  auto_timer_ = 0;
}

double Playback::auto_delay() const {
  if (line_had_voice_ || !presenter_->line()) {
    return options_.auto_base_delay;
  }
  return options_.auto_base_delay + options_.auto_seconds_per_glyph *
                                        static_cast<double>(presenter_->glyph_count()) *
                                        presenter_->line()->auto_multiplier;
}

// Notes a line that just came on screen: skip "seen" stops at one the player has not seen, and
// the line joins the seen set.
void Playback::observe_line() {
  if (presenter_->line_serial() == observed_serial_ || !presenter_->line()) {
    return;
  }
  observed_serial_ = presenter_->line_serial();
  auto_timer_ = 0;
  line_had_voice_ = false;
  const ScriptPosition position = runtime_->position();
  if (skip_ == SkipMode::kSeen && !persistent_->seen(position)) {
    skip_ = SkipMode::kOff;
  }
  persistent_->mark_seen(position);
}

void Playback::skip_step() {
  if (!presenter_->line()) {
    if (runtime_->state() == RunState::kWaiting) {
      skip_ = SkipMode::kOff;  // a menu or a screen: the player must act
    }
    return;
  }
  if (skip_timer_ < options_.skip_interval) {
    return;
  }
  skip_timer_ = 0;
  if (presenter_->revealing()) {
    presenter_->advance();  // shows the whole line
  }
  presenter_->advance();
  observe_line();
}

void Playback::auto_step(double seconds) {
  if (!presenter_->line()) {
    return;
  }
  if (options_.voice_playing && options_.voice_playing()) {
    line_had_voice_ = true;
    auto_timer_ = 0;
    return;
  }
  if (presenter_->revealing()) {
    return;
  }
  auto_timer_ += seconds;
  if (auto_timer_ >= auto_delay()) {
    presenter_->advance();
    observe_line();
  }
}

void Playback::update(double seconds) {
  for (const PlaybackRequest& request : runtime_->take_playback_requests()) {
    if (request.skip) {
      skip_ = *request.skip;
    }
    if (request.automatic) {
      set_auto(*request.automatic);
    }
  }
  observe_line();
  if (skip_ != SkipMode::kOff) {
    skip_timer_ += seconds;
    skip_step();
  } else if (auto_) {
    auto_step(seconds);
  }
}

}  // namespace vnmaki
