#pragma once

// Self-voicing (docs/SPEC.md section 17): speaks the story and the UI through a TtsBackend for a
// player who cannot see the screen. It speaks each new say or narrate line (the speaker, then the
// text without tags), the options of a menu, and the role, label and state of the node that gets
// the focus. New speech stops the old speech. It says nothing in skip mode. It needs no window:
// call update() once a frame, after the presenters have moved the story.

#include <cstdint>
#include <optional>
#include <string>

#include "engine/platform/tts.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/ui/ui_host.hpp"

namespace vnmaki {

class DialoguePresenter;
class Playback;

class SelfVoicing {
 public:
  /// All of them must outlive this. Off until set_enabled(true) (`prefs.self_voicing`).
  SelfVoicing(TtsBackend& tts, const ScriptRuntime& runtime, const DialoguePresenter& presenter,
              const Playback& playback, UiHost& ui);

  /// On: the line, menu or focused node of now is spoken at the next update(). Off: says
  /// "Self-voicing off" and then nothing.
  void set_enabled(bool enabled);
  [[nodiscard]] bool enabled() const { return enabled_; }

  /// Speaks the options of a menu in the language of `translator` (the line and the focused node
  /// are already translated by the presenter and the host). Must outlive this; null: source text.
  void set_translator(const Translator* translator) { translator_ = translator; }

  /// Speaks what changed since the last call.
  void update();

 private:
  void announce_line();
  [[nodiscard]] std::string focus_text();

  TtsBackend* tts_;
  const ScriptRuntime* runtime_;
  const DialoguePresenter* presenter_;
  const Playback* playback_;
  UiHost* ui_;
  const Translator* translator_ = nullptr;
  bool enabled_ = false;
  bool skipping_ = false;
  std::uint64_t line_serial_ = 0;
  // Where the menu that was spoken waited; empty when the story waits at no menu.
  std::optional<ScriptPosition> menu_position_{};
  // What was said for the focus last; empty for no focus.
  std::string focus_{};
};

}  // namespace vnmaki
