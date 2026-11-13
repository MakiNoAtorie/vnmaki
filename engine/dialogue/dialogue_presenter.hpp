#pragma once

// The dialogue presenter (docs/SPEC.md section 7): holds the line a script says or narrates, how
// much of it the typewriter has revealed, and what an "advance" input does. It needs no window,
// GPU or clock: the caller passes the elapsed seconds to update().

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace vnmaki {

class ScriptRuntime;

/// The typewriter speed unless the caller sets one.
inline constexpr double kDefaultCharactersPerSecond = 40;

/// One line on screen. An empty speaker is narration.
struct DialogueLine {
  std::string speaker{};
  /// The speaker's name colour as 0xRRGGBB.
  std::optional<std::uint32_t> color{};
  std::string text{};
  /// Scales the per-character part of the auto-advance delay (`character { auto_multiplier }`).
  double auto_multiplier = 1;

  friend bool operator==(const DialogueLine&, const DialogueLine&) = default;
};

/// Drives a started ScriptRuntime through its say and narrate lines. Advance the story only
/// through advance(); the presenter then notices the new line. It also notices a line that
/// replaced the old one without it (a load) at the next call. Menus and other requests are not
/// shown: line() is empty and advance() does nothing while the story waits at one.
class DialoguePresenter {
 public:
  /// Returns how many glyphs `line` has once laid out: what revealed_glyphs() counts up to. Called
  /// once for each line.
  using GlyphCounter = std::function<std::size_t(const DialogueLine&)>;

  /// Returns how many glyphs of `line` show `elapsed_seconds` after it started, at
  /// `characters_per_second`. Use it to count the `speed` and `pause` tags of the line's text.
  using Reveal = std::function<std::size_t(const DialogueLine&, double characters_per_second,
                                           double elapsed_seconds)>;

  /// `runtime` must outlive the presenter and be started already. Without `reveal`, every glyph
  /// takes the same time (revealed_glyphs of engine/text/text.hpp).
  DialoguePresenter(ScriptRuntime& runtime, GlyphCounter counter,
                    double characters_per_second = kDefaultCharactersPerSecond, Reveal reveal = {});

  /// The line being shown; empty when the story waits at something else or has ended.
  [[nodiscard]] const std::optional<DialogueLine>& line() const { return line_; }
  /// How many glyphs of line() show now; 0 without a line.
  [[nodiscard]] std::size_t visible_glyphs() const;
  /// How many glyphs line() has once laid out; 0 without a line.
  [[nodiscard]] std::size_t glyph_count() const { return line_ ? glyph_count_ : 0; }
  /// Counts the lines the presenter has picked up. It changes when line() shows another line,
  /// also when the story moved without advance() (a load).
  [[nodiscard]] std::uint64_t line_serial() const { return line_serial_; }
  /// True while part of line() is still hidden.
  [[nodiscard]] bool revealing() const;
  /// True once the story has finished or failed.
  [[nodiscard]] bool ended() const;

  /// Moves the reveal on by `seconds`.
  void update(double seconds);
  /// The player asked to go on. During a reveal this shows the whole line; otherwise the script
  /// moves on to its next request.
  void advance();

 private:
  void sync();

  ScriptRuntime* runtime_;
  GlyphCounter counter_;
  Reveal reveal_;
  double characters_per_second_;
  std::optional<DialogueLine> line_;
  std::size_t glyph_count_ = 0;
  double elapsed_ = 0;
  bool completed_ = false;
  std::uint64_t line_serial_ = 0;
};

}  // namespace vnmaki
