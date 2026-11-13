#include "engine/tools/inspect.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vnmaki {

namespace {

// The first step of a label: the first blocking call it reaches.
constexpr std::uint32_t kFirstStep = 1;

}  // namespace

std::vector<LabelRow> inspect_labels(const ScriptRuntime& runtime) {
  const bool waiting = runtime.state() == RunState::kWaiting;
  const std::string current = waiting ? runtime.position().label : std::string{};
  std::vector<LabelRow> rows;
  for (std::string& name : runtime.label_names()) {
    const bool is_current = waiting && name == current;
    rows.push_back({.name = std::move(name), .current = is_current});
  }
  return rows;
}

std::string jump_to_label(ScriptRuntime& runtime, std::string_view label, std::uint32_t step) {
  if (!runtime.has_label(label)) {
    return "no label '" + std::string{label} + "'";
  }
  if (step < kFirstStep) {
    return "a step starts at 1";
  }
  // Where the story is now, to go back to it if the label cannot reach step 1.
  const LabelSnapshot* entry = runtime.label_snapshot();
  const bool can_go_back = entry != nullptr;
  LabelSnapshot before = can_go_back ? *entry : LabelSnapshot{};
  const std::vector<InputResult> inputs{runtime.input_log().begin(), runtime.input_log().end()};
  const std::uint32_t step_before = runtime.position().step;

  LabelSnapshot target{.label = std::string{label},
                       .store = runtime.store_snapshot(),
                       .scene = entry != nullptr ? entry->scene : std::vector<std::byte>{}};
  runtime.drop_callers();
  runtime.replay(target, {}, step);
  if (runtime.state() != RunState::kFailed) {
    return {};
  }
  std::string problem =
      "'" + std::string{label} + "' did not reach step " + std::to_string(step) + ": " +
      runtime.error();
  if (can_go_back) {
    runtime.replay(before, inputs, step_before);
    if (runtime.state() == RunState::kFailed) {
      problem += "; and the story could not go back: " + runtime.error();
    }
  }
  return problem;
}

}  // namespace vnmaki
