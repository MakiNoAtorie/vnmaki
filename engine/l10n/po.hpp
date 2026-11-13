#pragma once

// A reader and a writer for gettext `.po` and `.pot` files (docs/SPEC.md section 16). It handles
// what the engine writes and what translation tools keep: comments, references, flags, `msgctxt`,
// `msgid`, `msgstr`, obsolete entries (`#~`) and C escapes. It has no plural forms. A `#|` line
// (the previous text of a fuzzy entry) is skipped.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vnmaki {

/// One message. The header of a file is the entry with an empty `context` and `id`; its `str` has
/// the lines `Name: value\n`.
struct PoEntry {
  std::vector<std::string> comments{};    ///< `# text`: the translator's notes.
  std::vector<std::string> extracted{};   ///< `#. text`: notes from the tool, e.g. the speaker.
  std::vector<std::string> references{};  ///< `#: file:line`, one word each.
  std::vector<std::string> flags{};       ///< `#, fuzzy`, one word each.
  std::string context{};                  ///< `msgctxt`. Empty: none.
  std::string id{};                       ///< `msgid`: the source text.
  std::string str{};                      ///< `msgstr`: the translation. Empty: none.
  bool obsolete = false;                  ///< The `msgctxt`, `msgid` and `msgstr` lines have `#~`.
  /// Set by `parse_po`: the line of the `msgid`. Not written and not compared.
  std::uint32_t line = 0;

  [[nodiscard]] bool fuzzy() const;
  void set_fuzzy(bool fuzzy);

  /// Equal in all fields but `line`.
  friend bool operator==(const PoEntry& a, const PoEntry& b);
};

struct PoFile {
  std::vector<PoEntry> entries{};

  friend bool operator==(const PoFile&, const PoFile&) = default;
};

/// The text is not valid `.po`. what() is the reason; line() is the line from 1.
class PoError : public std::runtime_error {
 public:
  PoError(std::uint32_t line, const std::string& reason)
      : std::runtime_error(reason), line_(line) {}
  [[nodiscard]] std::uint32_t line() const { return line_; }

 private:
  std::uint32_t line_ = 0;
};

/// Reads a `.po` text (LF or CRLF; a UTF-8 byte order mark at the start is skipped). Throws
/// PoError. A comment that no message follows is an error, so that a rewrite loses nothing.
[[nodiscard]] PoFile parse_po(std::string_view text);

/// Writes `file` (LF line ends) so that `parse_po` reads the same entries again. A text with a
/// line break is written as one string for each line.
[[nodiscard]] std::string write_po(const PoFile& file);

}  // namespace vnmaki
