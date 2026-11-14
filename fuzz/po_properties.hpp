#pragma once

// What must hold when parse_po and write_po (engine/l10n/po.hpp) meet any byte string. The fuzz
// target fuzz/po_fuzzer.cpp and a unit test (tests/unit/po_test.cpp) both run it, so the check
// itself is tested on compilers that build no fuzzers.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/l10n/po.hpp"

namespace vnmaki::fuzz {

inline void require_po(bool condition) {
  if (!condition) {
    std::abort();
  }
}

/// Reading any text either throws PoError with a line inside the text, or gives a file that
/// writes and reads back to itself, and whose written text is a fixed point.
inline void check_po_read_properties(std::string_view text) {
  PoFile parsed;
  try {
    parsed = parse_po(text);
  } catch (const PoError& error) {
    const auto lines = static_cast<std::size_t>(std::ranges::count(text, '\n')) + 1;
    require_po(error.line() >= 1 && error.line() <= lines);
    return;
  }
  const std::string written = write_po(parsed);
  PoFile again;
  try {
    again = parse_po(written);
  } catch (const PoError&) {
    require_po(false);  // what write_po wrote must be valid
  }
  require_po(again == parsed);
  require_po(write_po(again) == written);
}

inline std::string one_line(std::string text) {
  std::ranges::replace(text, '\n', ' ');
  while (!text.empty() && text.back() == '\r') {
    text.pop_back();
  }
  return text;
}

inline std::vector<std::string> words_of(const std::string& text) {
  std::vector<std::string> words;
  std::string word;
  for (const char c : text + ' ') {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (!word.empty()) {
        words.push_back(word);
      }
      word.clear();
    } else {
      word.push_back(c);
    }
  }
  return words;
}

/// A file made from the bytes (fields end at a 0xFF byte, five for each entry: context, text,
/// translation, a comment and a flag word) writes and reads back to itself. The texts are any
/// bytes, so this tests the escapes.
inline void check_po_write_properties(std::string_view bytes) {
  constexpr char kFieldEnd = '\xFF';
  constexpr std::size_t kFields = 5;
  std::vector<std::string> fields;
  std::string current;
  for (const char c : bytes) {
    if (c == kFieldEnd) {
      fields.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  fields.push_back(current);
  fields.resize((fields.size() + kFields - 1) / kFields * kFields);

  PoFile file;
  for (std::size_t i = 0; i < fields.size(); i += kFields) {
    PoEntry entry;
    entry.context = fields[i];
    entry.id = fields[i + 1];
    entry.str = fields[i + 2];
    entry.comments.push_back(one_line(fields[i + 3]));
    entry.references = words_of(fields[i + 3]);
    entry.extracted.push_back(one_line(fields[i + 4]));
    std::ranges::replace(fields[i + 4], ',', ';');  // a comma ends a flag
    entry.flags = words_of(fields[i + 4]);
    entry.obsolete = fields[i + 4].size() % 2 == 1;
    file.entries.push_back(std::move(entry));
  }
  PoFile again;
  try {
    again = parse_po(write_po(file));
  } catch (const PoError&) {
    require_po(false);
  }
  require_po(again == file);
}

}  // namespace vnmaki::fuzz
