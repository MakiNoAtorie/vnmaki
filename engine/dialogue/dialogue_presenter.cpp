#include "engine/dialogue/dialogue_presenter.hpp"

#include <cstddef>
#include <utility>
#include <variant>

#include "engine/runtime/script_runtime.hpp"
#include "engine/text/text.hpp"

namespace vnmaki {

DialoguePresenter::DialoguePresenter(ScriptRuntime& runtime, GlyphCounter counter,
                                     double characters_per_second)
    : runtime_(&runtime),
      counter_(std::move(counter)),
      characters_per_second_(characters_per_second) {
  sync();
}

std::size_t DialoguePresenter::visible_glyphs() const {
  if (!line_) {
    return 0;
  }
  return completed_ ? glyph_count_
                    : revealed_glyphs(glyph_count_, characters_per_second_, elapsed_);
}

bool DialoguePresenter::revealing() const { return line_ && visible_glyphs() < glyph_count_; }

bool DialoguePresenter::ended() const {
  const RunState state = runtime_->state();
  return state == RunState::kFinished || state == RunState::kFailed;
}

void DialoguePresenter::update(double seconds) {
  sync();
  if (line_) {
    elapsed_ += seconds;
  }
}

void DialoguePresenter::advance() {
  sync();
  if (!line_) {
    return;
  }
  if (revealing()) {
    completed_ = true;
    return;
  }
  line_.reset();
  runtime_->advance();
  sync();
}

// Picks up the request the script waits at, unless it is the line already shown.
void DialoguePresenter::sync() {
  if (line_) {
    return;
  }
  const BlockingRequest* request =
      runtime_->state() == RunState::kWaiting ? runtime_->request() : nullptr;
  if (const auto* say = request ? std::get_if<SayRequest>(request) : nullptr) {
    line_ = DialogueLine{.speaker = say->speaker, .color = say->color, .text = say->text};
  } else if (const auto* narrate = request ? std::get_if<NarrateRequest>(request) : nullptr) {
    line_ = DialogueLine{.speaker = {}, .color = std::nullopt, .text = narrate->text};
  } else {
    return;
  }
  glyph_count_ = counter_(*line_);
  elapsed_ = 0;
  completed_ = false;
}

}  // namespace vnmaki
