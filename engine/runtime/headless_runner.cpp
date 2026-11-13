#include "engine/runtime/headless_runner.hpp"

#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "engine/runtime/script_runtime.hpp"
#include "engine/saves/persistent_store.hpp"
#include "engine/saves/saves.hpp"

namespace vnmaki {

namespace {

// Writes each runtime event as one transcript line.
class TranscriptRecorder final : public ScriptObserver {
 public:
  explicit TranscriptRecorder(HeadlessResult& result) : result_(&result) {}

  void on_label_entered(std::string_view label) override { add("label ", label); }
  void on_label_resumed(std::string_view label) override { add("resume ", label); }
  void on_scene_command(std::string_view command) override { add("", command); }
  void on_audio_command(std::string_view command) override { add("", command); }
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
    } else if (const auto* screen = std::get_if<CallScreenRequest>(&request)) {
      add("screen ", screen->name);
    } else if (const auto* wait = std::get_if<WaitRequest>(&request)) {
      if (wait->tag == kPauseTag) {
        result_->transcript.push_back(std::format("pause {}", wait->seconds));
      } else {
        result_->transcript.push_back(wait->tag.empty() ? "wait_all" : "wait " + wait->tag);
      }
    } else if (const auto* movie = std::get_if<MovieRequest>(&request)) {
      add("movie ", movie->path);
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

// Runs the story and fills `result`. A separate function so that the persistent store, which
// writes its file when it goes away, is gone before the caller sees the result.
void run_into(HeadlessResult& result, const Vfs& vfs, std::span<const ScriptInput> inputs,
              const HeadlessOptions& options) {
  TranscriptRecorder recorder{result};
  ScriptRuntimeOptions runtime_options = options.runtime;
  runtime_options.observer = &recorder;
  ScriptRuntime runtime{vfs, runtime_options};

  // The persistent store and the saves answer the scripts' `save` and `load` calls.
  std::optional<PersistentStore> persistent;
  std::optional<Saves> saves;
  if (options.saves) {
    SavesOptions saves_options = *options.saves;
    if (!saves_options.on_warning) {
      saves_options.on_warning = [&result](std::string_view message) {
        result.transcript.push_back("warning " + std::string{message});
      };
    }
    persistent.emplace(runtime, saves_options.directory, saves_options.on_warning);
    persistent->load();
    saves.emplace(runtime, std::move(saves_options));
  }
  const auto poll = [&] {
    if (saves) {
      saves->poll();
      persistent->flush();
    }
  };

  try {
    runtime.load_scripts();
  } catch (const ScriptError& error) {
    result.outcome = HeadlessOutcome::kFailed;
    result.error = error.what();
    return;
  }

  runtime.start(options.start_label);
  poll();
  auto next_input = inputs.begin();
  while (runtime.state() == RunState::kWaiting) {
    if (const auto* wait = std::get_if<WaitRequest>(runtime.request())) {
      // No clock runs here: the tweens that end are at their end values, and the wait is over.
      if (runtime_options.scene != nullptr) {
        runtime_options.scene->finish_tweens(wait->tag);
      }
      runtime.advance();
      poll();
      continue;
    }
    if (std::holds_alternative<MovieRequest>(*runtime.request())) {
      runtime.advance();  // A movie is not played here, as in a replay.
      poll();
      continue;
    }
    if (next_input == inputs.end()) {
      break;
    }
    if (std::holds_alternative<RollbackInput>(*next_input)) {
      if (!runtime.roll_back()) {
        result.outcome = HeadlessOutcome::kFailed;
        result.error = "the next input is a rollback, but there is nothing to go back to";
        return;
      }
      ++next_input;
      poll();
      continue;
    }
    const auto* menu = std::get_if<MenuRequest>(runtime.request());
    const bool at_screen = std::holds_alternative<CallScreenRequest>(*runtime.request());
    const auto* choice = std::get_if<ChooseInput>(&*next_input);
    const auto* returned = std::get_if<ReturnScreenInput>(&*next_input);
    if (menu == nullptr && !at_screen && choice == nullptr && returned == nullptr) {
      runtime.advance();
    } else if (menu != nullptr && choice != nullptr && choice->index < menu->options.size()) {
      runtime.choose(choice->index);
    } else if (at_screen && returned != nullptr) {
      runtime.return_screen(returned->value);
    } else {
      result.outcome = HeadlessOutcome::kFailed;
      if (at_screen) {
        result.error = "the story waits at a screen, and the next input is not its result";
      } else if (menu == nullptr) {
        result.error = returned != nullptr
                           ? "the next input is a screen result, but the story does not wait at a "
                             "screen"
                           : "the next input is a menu choice, but the story does not wait at a "
                             "menu";
      } else {
        result.error = "the story waits at a menu, and the next input is not one of its options";
      }
      return;
    }
    ++next_input;
    poll();
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
}

}  // namespace

HeadlessResult run_headless(const Vfs& vfs, std::span<const ScriptInput> inputs,
                            const HeadlessOptions& options) {
  HeadlessResult result;
  run_into(result, vfs, inputs, options);
  return result;
}

}  // namespace vnmaki
