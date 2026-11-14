#pragma once

// The backlog (docs/SPEC.md section 8): the lines the player has read, newest last, with voice
// replay. It keeps no list of its own. It reads the rollback step log of the runtime, so it is
// right after a rollback and after a load too (a load starts the log, and so the backlog, again).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/l10n/translator.hpp"
#include "engine/runtime/script_runtime.hpp"

namespace vnmaki {

struct BacklogEntry {
  /// Empty for a narrate line.
  std::string speaker{};
  /// The line with text tags, translated if the backlog has a translator.
  std::string text{};
  /// The label that said the line, and its step there.
  std::string label{};
  std::uint32_t step = 0;
  /// The audio name of the line's `voice(name)`; empty if it had none.
  std::string voice{};

  friend bool operator==(const BacklogEntry&, const BacklogEntry&) = default;
};

class Backlog {
 public:
  /// `runtime` must outlive this. `audio` (may be null) plays voices again; it must outlive this.
  /// With a `translator` (may be null; it must outlive this) the entries have the translated text
  /// of its language (scope: the label of the entry), so a change of language shows in the backlog.
  explicit Backlog(const ScriptRuntime& runtime, AudioCommands* audio = nullptr,
                   std::size_t max_entries = kDefaultBacklogLength,
                   const Translator* translator = nullptr)
      : runtime_(&runtime), audio_(audio), max_entries_(max_entries), translator_(translator) {}

  /// The revision of the translator (0 without one): what entries() is translated for.
  [[nodiscard]] std::uint64_t revision() const {
    return translator_ != nullptr ? translator_->revision() : 0;
  }

  /// The say and narrate lines of the step log, oldest first, at most `max_entries` of the
  /// newest. The log keeps ScriptRuntimeOptions::rollback_depth steps, so that bounds it too.
  [[nodiscard]] std::vector<BacklogEntry> entries() const;

  /// Plays the voice of `entry` again. Returns why it cannot (the file is gone), or an empty
  /// string; also empty, and nothing played, if the entry has no voice or there is no audio.
  [[nodiscard]] std::string play_voice(const BacklogEntry& entry) const;

 private:
  const ScriptRuntime* runtime_;
  AudioCommands* audio_;
  std::size_t max_entries_;
  const Translator* translator_;
};

}  // namespace vnmaki
