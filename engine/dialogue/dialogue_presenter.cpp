#include "engine/dialogue/dialogue_presenter.hpp"

#include <cstddef>
#include <optional>
#include <utility>
#include <variant>

#include "engine/runtime/script_runtime.hpp"
#include "engine/text/text.hpp"

namespace vnmaki {

DialoguePresenter::DialoguePresenter(ScriptRuntime& runtime, GlyphCounter counter,
                                     double characters_per_second, Reveal reveal)
    : runtime_(&runtime),
      counter_(std::move(counter)),
      reveal_(std::move(reveal)),
      characters_per_second_(characters_per_second) {
  sync();
}

std::size_t DialoguePresenter::visible_glyphs() const {
  if (!line_) {
    return 0;
  }
  if (completed_) {
    return glyph_count_;
  }
  return reveal_ ? reveal_(*line_, characters_per_second_, elapsed_)
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

// Picks up the request the script waits at, unless it is the line already shown. A request that
// is not the line shown (the story moved by itself, e.g. a load) replaces it.
void DialoguePresenter::sync() {
  const BlockingRequest* request =
      runtime_->state() == RunState::kWaiting ? runtime_->request() : nullptr;
  std::optional<DialogueLine> next;
  if (const auto* say = request ? std::get_if<SayRequest>(request) : nullptr) {
    next = DialogueLine{.speaker = say->speaker,
                        .color = say->color,
                        .text = say->text,
                        .auto_multiplier = say->auto_multiplier};
  } else if (const auto* narrate = request ? std::get_if<NarrateRequest>(request) : nullptr) {
    next = DialogueLine{.speaker = {}, .color = std::nullopt, .text = narrate->text};
  }
  if (next == line_) {
    return;
  }
  line_ = std::move(next);
  if (!line_) {
    return;
  }
  ++line_serial_;
  glyph_count_ = counter_(*line_);
  elapsed_ = 0;
  completed_ = false;
}

}  // namespace vnmaki
