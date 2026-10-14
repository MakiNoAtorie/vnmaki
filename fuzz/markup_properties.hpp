#pragma once

// What must hold when parse_markup (engine/text/markup.hpp) reads any byte string. The fuzz target
// fuzz/markup_fuzzer.cpp and a unit test (tests/unit/markup_test.cpp) both run it, so the check
// itself is tested on compilers that build no fuzzers.

#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>

#include "engine/text/markup.hpp"

namespace vnmaki::fuzz {

inline void require_markup(bool condition) {
  if (!condition) {
    std::abort();
  }
}

/// Calls std::abort() unless:
/// - parsing gives a result or a MarkupError whose offset is a "{" of the input;
/// - the plain text is not longer than the input, and holds a "{" only where the input has "{{";
/// - the runs are not empty and cover the plain text in order, without gaps;
/// - pauses lie in the text, in order;
/// - the escaped plain text parses back to the same plain text, as one run without style.
inline void check_markup_properties(std::string_view markup) {
  StyledText styled;
  try {
    styled = parse_markup(markup);
  } catch (const MarkupError& error) {
    require_markup(error.offset() < markup.size() && markup[error.offset()] == '{');
    require_markup(!std::string_view{error.what()}.empty());
    return;
  }
  require_markup(styled.text.size() <= markup.size());
  if (markup.find("{{") == std::string_view::npos) {
    require_markup(styled.text.find('{') == std::string::npos);
  }

  std::size_t covered = 0;
  for (const StyledRun& run : styled.runs) {
    require_markup(run.begin == covered && run.end > run.begin);
    covered = run.end;
  }
  require_markup(covered == styled.text.size());

  std::size_t last_pause = 0;
  for (const TextPause& pause : styled.pauses) {
    require_markup(pause.offset >= last_pause && pause.offset <= styled.text.size());
    require_markup(pause.seconds >= 0 && pause.seconds <= kMaxTagNumber);
    last_pause = pause.offset;
  }

  const StyledText again = parse_markup(escape_markup(styled.text));
  require_markup(again == plain_text(styled.text));
}

}  // namespace vnmaki::fuzz
