#pragma once

// Inline text tags (docs/TEXT_TAGS.md): "I {color=#f00}really{/color} mean it." becomes plain
// text and the styles of its parts. Parsing needs no font.

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "engine/render/renderer.hpp"

namespace vnmaki {

/// The largest font size, in pixels per em, that tags, layout and the glyph atlas accept.
inline constexpr float kMaxFontPixelSize = 1024;
/// The largest `speed` (characters per second) and `pause` (seconds) a tag may give.
inline constexpr double kMaxTagNumber = 100000;

/// How one part of a text is drawn and revealed. The default is "as the text around it".
struct TextStyle {
  /// `{color=#rrggbb}`. Not set: the colour the text is drawn with.
  std::optional<Color> color;
  /// `{size=N}`: pixels per em. 0: the size of the layout.
  float size = 0;
  /// `{font=name}`: a family of the FontSet. Empty: the base family.
  std::string font;
  /// `{b}` and `{i}`.
  bool bold = false;
  bool italic = false;
  /// `{speed=N}`: characters per second of the typewriter reveal. 0: the speed of the reveal.
  double speed = 0;
  /// `{ruby=text}`: small text (furigana) drawn above this part.
  std::string ruby;
  /// `{link=id}`: this part can be clicked, and reports `id`.
  std::string link;

  bool operator==(const TextStyle&) const = default;
};

/// The bytes [begin, end) of StyledText::text and their style.
struct StyledRun {
  std::size_t begin = 0;
  std::size_t end = 0;
  TextStyle style;

  bool operator==(const StyledRun&) const = default;
};

/// `{pause=N}`: the reveal waits `seconds` before it shows the character at byte `offset`.
struct TextPause {
  std::size_t offset = 0;
  double seconds = 0;

  bool operator==(const TextPause&) const = default;
};

struct StyledText {
  /// The text without its tags (UTF-8).
  std::string text;
  /// In text order, without gaps or empty runs; together they cover all of `text`.
  std::vector<StyledRun> runs;
  /// In text order.
  std::vector<TextPause> pauses;

  bool operator==(const StyledText&) const = default;
};

/// A tag that cannot be used. what() names the reason; offset() is the byte in the markup where
/// the tag starts.
class MarkupError : public std::runtime_error {
 public:
  MarkupError(std::size_t offset, const std::string& reason)
      : std::runtime_error(reason), offset_(offset) {}
  [[nodiscard]] std::size_t offset() const { return offset_; }

 private:
  std::size_t offset_ = 0;
};

/// Reads the tags of `markup` (see docs/TEXT_TAGS.md). Throws MarkupError.
[[nodiscard]] StyledText parse_markup(std::string_view markup);

/// `text` with every "{" written as "{{", so parse_markup gives `text` back without styles.
[[nodiscard]] std::string escape_markup(std::string_view text);

/// `text` as one run of `style`: what layout takes for text without tags.
[[nodiscard]] StyledText plain_text(std::string_view text, TextStyle style = {});

}  // namespace vnmaki
