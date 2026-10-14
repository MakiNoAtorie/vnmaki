#include "engine/runtime/headless_runner.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "engine/runtime/script_runtime.hpp"

namespace vnmaki {

namespace {

// Writes each runtime event as one transcript line.
class TranscriptRecorder final : public ScriptObserver {
 public:
  explicit TranscriptRecorder(HeadlessResult& result) : result_(&result) {}

  void on_label_entered(std::string_view label) override { add("label ", label); }
  void on_label_resumed(std::string_view label) override { add("resume ", label); }
  void on_scene_command(std::string_view command) override { add("", command); }
  void on_print(std::string_view text) override { add("print ", text); }
  void on_warning(std::string_view message) override { add("warning ", message); }

  void on_blocked(const BlockingRequest& request, const ScriptPosition& position) override {
    result_->stops.push_back(position);
    if (const auto* say = std::get_if<SayRequest>(&request)) {
      result_->transcript.push_back("say " + say->speaker + ": " + say->text);
    } else if (const auto* narrate = std::get_if<NarrateRequest>(&request)) {
      add("narrate ", narrate->text);
    } else if (const auto* menu = std::get_if<MenuRequest>(&request)) {
      for (std::size_t i = 0; i < menu->options.size(); ++i) {
        result_->transcript.push_back("menu " + std::to_string(i + 1) + ": " + menu->options[i]);
      }
    }
  }

 private:
  void add(std::string_view prefix, std::string_view text) {
    std::string line{prefix};
    line += text;
    result_->transcript.push_back(std::move(line));
  }

  HeadlessResult* result_;
};

}  // namespace

HeadlessResult run_headless(const Vfs& vfs, std::span<const ScriptInput> inputs,
                            const HeadlessOptions& options) {
  HeadlessResult result;
  TranscriptRecorder recorder{result};
  ScriptRuntimeOptions runtime_options = options.runtime;
  runtime_options.observer = &recorder;
  ScriptRuntime runtime{vfs, runtime_options};

  try {
    runtime.load_scripts();
  } catch (const ScriptError& error) {
    result.outcome = HeadlessOutcome::kFailed;
    result.error = error.what();
    return result;
  }

  runtime.start(options.start_label);
  auto next_input = inputs.begin();
  while (runtime.state() == RunState::kWaiting && next_input != inputs.end()) {
    const auto* menu = std::get_if<MenuRequest>(runtime.request());
    const auto* choice = std::get_if<ChooseInput>(&*next_input);
    if (menu == nullptr && choice == nullptr) {
      runtime.advance();
    } else if (menu != nullptr && choice != nullptr && choice->index < menu->options.size()) {
      runtime.choose(choice->index);
    } else {
      result.outcome = HeadlessOutcome::kFailed;
      result.error =
          menu == nullptr
              ? "the next input is a menu choice, but the story does not wait at a menu"
              : "the story waits at a menu, and the next input is not one of its options";
      return result;
    }
    ++next_input;
  }

  switch (runtime.state()) {
    case RunState::kFinished:
      result.outcome = HeadlessOutcome::kFinished;
      break;
    case RunState::kWaiting:
      result.outcome = HeadlessOutcome::kOutOfInput;
      break;
    case RunState::kIdle:
    case RunState::kFailed:
      result.outcome = HeadlessOutcome::kFailed;
      result.error = runtime.error();
      break;
  }
  return result;
}

}  // namespace vnmaki
