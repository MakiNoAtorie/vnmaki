#pragma once

// The headless runner: runs a game's scripts with no window, feeding them
// scripted player inputs, and records what they asked for as a transcript.
// Tests drive script behaviour through it, and `vnmaki run --headless` uses it.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "engine/runtime/script_runtime.hpp"
#include "engine/saves/saves.hpp"

namespace vnmaki {

class Vfs;

/// The player advances past the current line.
struct AdvanceInput {};

/// The player picks option `index` (from 0) of the current menu.
struct ChooseInput {
  std::size_t index = 0;
};

/// The screen a `call_screen` waits at returns `value`.
struct ReturnScreenInput {
  ScreenValue value{};
};

/// The player rolls back one say, narrate, menu or call_screen (ScriptRuntime::roll_back()). The
/// transcript then shows the request the story went back to. It fails the run if there is
/// nowhere to go back to.
struct RollbackInput {};

/// One scripted player input.
using ScriptInput = std::variant<AdvanceInput, ChooseInput, ReturnScreenInput, RollbackInput>;

enum class HeadlessOutcome : std::uint8_t {
  kFinished,    ///< The story ended.
  kOutOfInput,  ///< The script still waits, but every input was used.
  kFailed,      ///< A script failed to load or raised an error.
};

/// The transcript has one line per event, in order:
///
///   label <name>          a label was entered (start, jump or call)
///   resume <name>         a called label returned to its caller <name>
///   say <who>: <text>     say(who, text)
///   narrate <text>        narrate(text), or say(nil, text)
///   menu <n>: <text>      option n (from 1) of a menu; one line per option
///   screen <name>         call_screen(name)
///   scene <image>         scene(image)
///   show <tag> ...        show(tag, options), then the options given (see
///                         ScriptObserver::on_scene_command)
///   hide <tag>            hide(tag)
///   tween <tag>           tween(tag, options); also "sequence" and "parallel" for a group
///   wait <tag>            wait(tag); "wait_all" for wait_all()
///   music <name> ...      music(name, options), then the options given (see
///                         ScriptObserver::on_audio_command, also for the next four)
///   sound <name> ...      sound(name, options)
///   voice <name>          voice(name)
///   stop <channel> ...    stop(channel, options)
///   volume <channel> <n>  volume(channel, n)
///   print <text>          print(...), arguments joined by tabs
///   warning <message>     a dev-mode warning
struct HeadlessResult {
  HeadlessOutcome outcome = HeadlessOutcome::kFailed;
  std::vector<std::string> transcript;
  /// The position of every yield point reached, in order (one per say or
  /// narrate line, menu and wait in the transcript).
  std::vector<ScriptPosition> stops;
  std::string error;  ///< Why it failed; empty otherwise.
};

struct HeadlessOptions {
  std::string start_label = "start";
  /// The observer field is ignored: the runner observes the runtime itself.
  ScriptRuntimeOptions runtime;
  /// Set: the runner answers `save`, `load`, `quick_save` and `quick_load`, writes the auto-save
  /// slot, and keeps `persistent` in `saves->directory`. Without it those calls do nothing.
  /// Warnings of the saves go to the transcript unless `on_warning` is set.
  std::optional<SavesOptions> saves;
};

/// Loads every script under `scripts/` in `vfs`, enters the start label and
/// answers each yield point with the next input until the story ends, the
/// inputs run out or a script fails. A wait needs no input: no clock runs, so
/// the tweens it waits for (SceneCommands::finish_tweens) end at once. A rollback goes back over
/// waits too. An
/// input that does not fit the yield point (an advance at a menu or a screen,
/// a choice at a line or of an option the menu does not have, a screen result
/// anywhere else) also fails the run.
[[nodiscard]] HeadlessResult run_headless(const Vfs& vfs, std::span<const ScriptInput> inputs,
                                          const HeadlessOptions& options = {});

}  // namespace vnmaki
