#include "engine/dialogue/backlog.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace vnmaki {

namespace {

// The entry of a step that waited at a say or narrate; none for a menu or a screen.
std::optional<BacklogEntry> to_entry(const RollbackStep& step) {
  BacklogEntry entry{.voice = step.voice};
  if (const auto* say = std::get_if<SayRequest>(&step.request)) {
    entry.speaker = say->speaker;
    entry.text = say->text;
  } else if (const auto* narrate = std::get_if<NarrateRequest>(&step.request)) {
    entry.text = narrate->text;
  } else {
    return std::nullopt;
  }
  if (!step.stack.empty()) {
    entry.label = step.stack.back().label;
    entry.step = step.stack.back().step;
  }
  return entry;
}

}  // namespace

std::vector<BacklogEntry> Backlog::entries() const {
  std::vector<BacklogEntry> result;
  const std::span<const RollbackStep> steps = runtime_->rollback_steps();
  result.reserve(steps.size());
  for (const RollbackStep& step : steps) {
    if (std::optional<BacklogEntry> entry = to_entry(step)) {
      result.push_back(std::move(*entry));
    }
  }
  if (result.size() > max_entries_) {
    result.erase(result.begin(),
                 result.begin() + static_cast<std::ptrdiff_t>(result.size() - max_entries_));
  }
  return result;
}

std::string Backlog::play_voice(const BacklogEntry& entry) const {
  if (audio_ == nullptr || entry.voice.empty()) {
    return {};
  }
  return audio_->voice(entry.voice);
}

}  // namespace vnmaki
