// The `.po` reader and writer (engine/l10n/po.hpp). The expected text is written out by hand.

#include "engine/l10n/po.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "fuzz/po_properties.hpp"

namespace {

using vnmaki::PoEntry;
using vnmaki::PoError;
using vnmaki::PoFile;

PoEntry entry(std::string context, std::string id, std::string str) {
  PoEntry result;
  result.context = std::move(context);
  result.id = std::move(id);
  result.str = std::move(str);
  return result;
}

// The error of reading `text`: its line, or 0 when it reads.
std::uint32_t error_line(std::string_view text) {
  try {
    static_cast<void>(vnmaki::parse_po(text));
  } catch (const PoError& error) {
    return error.line();
  }
  return 0;
}

constexpr std::array<std::string_view, 12> kPropertySamples = {
    "",
    "msgid \"\"\nmsgstr \"\"\n",
    "# note\n#. speaker: a\n#: x.lua:1 y.lua:2\n#, fuzzy, c-format\nmsgctxt \"k\"\nmsgid \"a\"\n"
    "msgstr \"b\"\n",
    "#~ msgctxt \"k\"\n#~ msgid \"a\"\n#~ msgstr \"b\"\n",
    "msgid \"a\\n\"\n\"b\\n\"\nmsgstr \"\"\n",
    "msgid \"\\033\\x41\\101\\t\\\"\\\\\"\nmsgstr \"\\0\"\n",
    "msgid \"a\"",
    "msgid \"a\"\nmsgstr \"b",
    "# only a comment\n",
    "\xEF\xBB\xBFmsgid \"x\"\r\nmsgstr \"y\"\r\n",
    "msgstr \"a\"\nmsgid \"b\"\n",
    "msgid \"a\"\nmsgstr \"b\"\n#~| msgid \"c\"\n#| msgid \"d\"\n"};

// A fixed generator (Knuth's MMIX constants), so a failure repeats on every platform.
class Generator {
 public:
  explicit Generator(std::uint64_t seed) : state_(seed) {}

  std::size_t below(std::size_t limit) {
    constexpr std::uint64_t kMultiplier = 6364136223846793005ULL;
    constexpr std::uint64_t kIncrement = 1442695040888963407ULL;
    constexpr unsigned kDiscardedLowBits = 33;
    state_ = state_ * kMultiplier + kIncrement;
    return static_cast<std::size_t>((state_ >> kDiscardedLowBits) % limit);
  }

 private:
  std::uint64_t state_;
};

constexpr std::array<std::string_view, 24> kFragments = {
    "msgid ", "msgstr ", "msgctxt ", "\"", "\"\"", "\"a\"", "\"b\\n\"", "\\", "\\n", "\\x4",
    "\\07", "\n", "\n\n", "#~ ", "# ", "#. ", "#: a:1 ", "#, fuzzy", "#| ", "\r\n", "\xFF",
    " ", "z", "\t"};

constexpr std::uint64_t kSeed = 0x90A;
constexpr std::size_t kRandomCases = 3000;
constexpr std::size_t kMaxFragments = 30;

}  // namespace

TEST_CASE("po: a file is written in the gettext layout") {
  PoFile file;
  PoEntry header = entry("", "", "Content-Type: text/plain; charset=UTF-8\nLanguage: ja\n");
  PoEntry message = entry("start/1a2b3c4d", "Say \"hi\"\tnow\\", "\xE3\x81\x93");
  message.comments = {"check this"};
  message.extracted = {"speaker: eileen"};
  message.references = {"scripts/main.lua:3", "scripts/main.lua:9"};
  message.flags = {"fuzzy"};
  PoEntry gone = entry("", "Old", "Alt");
  gone.obsolete = true;
  file.entries = {header, message, entry("a/b", "line one\nline two\n", ""), gone};
  const std::string expected =
      "msgid \"\"\n"
      "msgstr \"\"\n"
      "\"Content-Type: text/plain; charset=UTF-8\\n\"\n"
      "\"Language: ja\\n\"\n"
      "\n"
      "# check this\n"
      "#. speaker: eileen\n"
      "#: scripts/main.lua:3 scripts/main.lua:9\n"
      "#, fuzzy\n"
      "msgctxt \"start/1a2b3c4d\"\n"
      "msgid \"Say \\\"hi\\\"\\tnow\\\\\"\n"
      "msgstr \"\xE3\x81\x93\"\n"
      "\n"
      "msgctxt \"a/b\"\n"
      "msgid \"\"\n"
      "\"line one\\n\"\n"
      "\"line two\\n\"\n"
      "msgstr \"\"\n"
      "\n"
      "#~ msgid \"Old\"\n"
      "#~ msgstr \"Alt\"\n";
  CHECK(vnmaki::write_po(file) == expected);
  CHECK(vnmaki::parse_po(expected) == file);
}

TEST_CASE("po: the reader accepts what other tools write") {
  const PoFile file = vnmaki::parse_po(
      "\xEF\xBB\xBF# Copyright\r\n"
      "msgid \"\"\r\n"
      "msgstr \"\"\r\n"
      "\"Language: ja\\n\"\r\n"
      "\r\n"
      "#: a.lua:1\r\n"
      "#, fuzzy,   c-format\r\n"
      "#| msgid \"old\"\r\n"
      "msgctxt   \"k\"\r\n"
      "msgid\t\"a\\x41\\101\\a\"   \r\n"
      "msgstr \"\"\r\n"
      "\"x\"\r\n"
      "\"y\"\r\n"
      "#~| msgid \"gone\"\r\n"
      "#~ msgid \"o\"\r\n"
      "#~ msgstr \"p\"\r\n");
  REQUIRE(file.entries.size() == 3);
  CHECK(file.entries[0].comments == std::vector<std::string>{"Copyright"});
  CHECK(file.entries[0].str == "Language: ja\n");
  CHECK(file.entries[1].references == std::vector<std::string>{"a.lua:1"});
  CHECK(file.entries[1].flags == std::vector<std::string>{"fuzzy", "c-format"});
  CHECK(file.entries[1].fuzzy());
  CHECK(file.entries[1].context == "k");
  CHECK(file.entries[1].id == "aAA\a");
  CHECK(file.entries[1].str == "xy");
  CHECK(file.entries[1].line == 10);
  CHECK(file.entries[2].obsolete);
  CHECK(file.entries[2].id == "o");
  CHECK(file.entries[2].str == "p");
}

TEST_CASE("po: an entry can follow another with no blank line") {
  const PoFile file = vnmaki::parse_po(
      "msgid \"a\"\nmsgstr \"1\"\n# note\nmsgid \"b\"\nmsgstr \"2\"\nmsgid \"c\"\nmsgstr \"3\"\n");
  REQUIRE(file.entries.size() == 3);
  CHECK(file.entries[1].comments == std::vector<std::string>{"note"});
  CHECK(file.entries[2].id == "c");
}

TEST_CASE("po: set_fuzzy adds and removes the flag once") {
  PoEntry e = entry("", "a", "b");
  e.set_fuzzy(true);
  e.set_fuzzy(true);
  CHECK(e.flags == std::vector<std::string>{"fuzzy"});
  e.set_fuzzy(false);
  CHECK_FALSE(e.fuzzy());
  CHECK(e.flags.empty());
}

TEST_CASE("po: a text that is not valid .po gives the line of the problem") {
  CHECK(error_line("msgid \"a\"") == 1);                                   // no msgstr
  CHECK(error_line("msgid \"a\"\nmsgstr \"b") == 2);                       // not closed
  CHECK(error_line("msgid \"a\"\nmsgstr \"b\" x\n") == 2);                 // text after the quote
  CHECK(error_line("\n\nmsgstr \"a\"\n") == 3);                            // no msgid
  CHECK(error_line("msgid \"a\"\nmsgid \"b\"\n") == 2);                    // two msgid
  CHECK(error_line("msgid a\nmsgstr \"\"\n") == 1);                        // no quote
  CHECK(error_line("\"a\"\n") == 1);                                       // nothing to continue
  CHECK(error_line("msgid_plural \"a\"\n") == 1);                          // plurals: no
  CHECK(error_line("msgid \"\\q\"\nmsgstr \"\"\n") == 1);                  // unknown escape
  CHECK(error_line("msgid \"\\777\"\nmsgstr \"\"\n") == 1);                // beyond a byte
  CHECK(error_line("msgid \"\\xg\"\nmsgstr \"\"\n") == 1);                 // no hex digit
  CHECK(error_line("msgid \"a\"\nmsgstr \"b\"\n\n# lost\n") == 4);         // comment, no message
  CHECK(error_line("# a\nmsgctxt \"c\"\n# b\nmsgid \"x\"\nmsgstr \"\"\n") == 3);  // comment inside
  CHECK(error_line("msgid \"a\"\nmsgstr \"b\"\n") == 0);
}

TEST_CASE("po: the properties of the fuzz target hold") {
  for (const std::string_view sample : kPropertySamples) {
    CHECK_NOTHROW(vnmaki::fuzz::check_po_read_properties(sample));
    CHECK_NOTHROW(vnmaki::fuzz::check_po_write_properties(sample));
  }
}

TEST_CASE("po: the properties hold on random texts") {
  Generator random{kSeed};
  for (std::size_t i = 0; i < kRandomCases; ++i) {
    std::string text;
    const std::size_t count = random.below(kMaxFragments);
    for (std::size_t j = 0; j < count; ++j) {
      text += kFragments.at(random.below(kFragments.size()));
    }
    vnmaki::fuzz::check_po_read_properties(text);
    vnmaki::fuzz::check_po_write_properties(text);
  }
  CHECK(true);  // an abort in the loop fails the run
}

TEST_CASE("po: every byte survives a write and a read") {
  std::string all;
  for (int byte = 0; byte < 256; ++byte) {
    all.push_back(static_cast<char>(byte));
  }
  PoFile file;
  file.entries.push_back(entry(all, all + all, all));
  CHECK(vnmaki::parse_po(vnmaki::write_po(file)) == file);
}
