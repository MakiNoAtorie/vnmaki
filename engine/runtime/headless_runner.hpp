#pragma once

// The headless runner: runs a game's scripts with no window, feeding them
// scripted player inputs, and records what they asked for as a transcript.
// Tests drive script behaviour through it, and `vnmaki run --headless` uses it.

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "engine/runtime/script_runtime.hpp"

namespace vnmaki {

class Vfs;

/// The player advances past the current line.
struct AdvanceInput {};

/// One scripted player input. Later tickets add menu choices.
using ScriptInput = std::variant<AdvanceInput>;

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
///   print <text>          print(...), arguments joined by tabs
///   warning <message>     a dev-mode warning
struct HeadlessResult {
  HeadlessOutcome outcome = HeadlessOutcome::kFailed;
  std::vector<std::string> transcript;
  /// The position of every yield point reached, in order (one per say or
  /// narrate line in the transcript).
  std::vector<ScriptPosition> stops;
  std::string error;  ///< Why it failed; empty otherwise.
};

struct HeadlessOptions {
  std::string start_label = "start";
  /// The observer field is ignored: the runner observes the runtime itself.
  ScriptRuntimeOptions runtime;
};

/// Loads every script under `scripts/` in `vfs`, enters the start label and
/// answers each yield point with the next input until the story ends, the
/// inputs run out or a script fails.
[[nodiscard]] HeadlessResult run_headless(const Vfs& vfs, std::span<const ScriptInput> inputs,
                                          const HeadlessOptions& options = {});

}  // namespace vnmaki
