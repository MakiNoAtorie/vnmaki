#include "engine/l10n/po.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vnmaki {

namespace {

constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";
constexpr std::string_view kFuzzy = "fuzzy";
constexpr int kOctalBase = 8;
constexpr int kHexBase = 16;
constexpr int kMaxOctalDigits = 3;
constexpr int kMaxHexDigits = 2;
constexpr unsigned kMaxByte = 255;
constexpr char kDelete = 0x7f;
constexpr char kBell = 7;
constexpr char kBackspace = 8;
constexpr char kVerticalTab = 11;
constexpr char kFormFeed = 12;

bool is_blank(char c) { return c == ' ' || c == '\t'; }

std::string_view skip_blanks(std::string_view text) {
  while (!text.empty() && is_blank(text.front())) {
    text.remove_prefix(1);
  }
  return text;
}

std::string_view trim_blanks(std::string_view text) {
  text = skip_blanks(text);
  while (!text.empty() && is_blank(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

// The value of a digit in `base`, or -1.
int digit_value(char c, int base) {
  int value = -1;
  if (c >= '0' && c <= '9') {
    value = c - '0';
  } else if (c >= 'a' && c <= 'f') {
    value = c - 'a' + 10;
  } else if (c >= 'A' && c <= 'F') {
    value = c - 'A' + 10;
  }
  return value < base ? value : -1;
}

// The escape at text[i] (after the backslash). Appends its byte and moves `i` past it.
void read_escape(std::string_view text, std::size_t& i, std::string& out, std::uint32_t line) {
  const char c = text[i];
  const auto simple = [&](char byte) {
    out.push_back(byte);
    ++i;
  };
  switch (c) {
    case 'n': return simple('\n');
    case 't': return simple('\t');
    case 'r': return simple('\r');
    case 'a': return simple(kBell);
    case 'b': return simple(kBackspace);
    case 'f': return simple(kFormFeed);
    case 'v': return simple(kVerticalTab);
    case '\\': return simple('\\');
    case '"': return simple('"');
    default: break;
  }
  const bool hex = c == 'x';
  const int base = hex ? kHexBase : kOctalBase;
  const int max_digits = hex ? kMaxHexDigits : kMaxOctalDigits;
  if (hex) {
    ++i;
  } else if (digit_value(c, base) < 0) {
    throw PoError(line, std::string{"unknown escape '\\"} + c + "'");
  }
  unsigned value = 0;
  int digits = 0;
  while (digits < max_digits && i < text.size() && digit_value(text[i], base) >= 0) {
    value = value * static_cast<unsigned>(base) + static_cast<unsigned>(digit_value(text[i], base));
    ++i;
    ++digits;
  }
  if (digits == 0) {
    throw PoError(line, "\\x needs a hex digit");
  }
  if (value > kMaxByte) {
    throw PoError(line, "octal escape is larger than 255");
  }
  out.push_back(static_cast<char>(static_cast<unsigned char>(value)));
}

// `text` starts with a quote: the string up to the closing quote, unescaped.
std::string parse_quoted(std::string_view text, std::uint32_t line) {
  std::string out;
  std::size_t i = 1;
  while (true) {
    if (i >= text.size()) {
      throw PoError(line, "string is not closed");
    }
    const char c = text[i];
    if (c == '"') {
      ++i;
      break;
    }
    if (c != '\\') {
      out.push_back(c);
      ++i;
      continue;
    }
    ++i;
    if (i >= text.size()) {
      throw PoError(line, "string is not closed");
    }
    read_escape(text, i, out, line);
  }
  if (!trim_blanks(text.substr(i)).empty()) {
    throw PoError(line, "text after the closing quote");
  }
  return out;
}

// One word after another, split at blanks (`split_at_commas` false) or commas.
std::vector<std::string> split_words(std::string_view text, bool split_at_commas) {
  std::vector<std::string> words;
  std::string word;
  const auto end_word = [&] {
    if (!word.empty()) {
      words.push_back(std::move(word));
      word.clear();
    }
  };
  for (const char c : text) {
    if (split_at_commas ? c == ',' : is_blank(c)) {
      end_word();
    } else {
      word.push_back(c);
    }
  }
  end_word();
  if (split_at_commas) {
    for (std::string& item : words) {
      item = std::string{trim_blanks(item)};
    }
    std::erase(words, std::string{});
  }
  return words;
}

enum class Field : std::uint8_t { kNone, kContext, kId, kStr };

class Parser {
 public:
  PoFile run(std::string_view text) {
    if (text.starts_with(kByteOrderMark)) {
      text.remove_prefix(kByteOrderMark.size());
    }
    while (!text.empty()) {
      const std::size_t end = std::min(text.find('\n'), text.size());
      std::string_view line = text.substr(0, end);
      while (line.ends_with('\r')) {
        line.remove_suffix(1);
      }
      ++line_;
      handle(skip_blanks(line));
      text.remove_prefix(std::min(end + 1, text.size()));
    }
    flush();
    return std::move(file_);
  }

 private:
  void handle(std::string_view s) {
    if (s.empty()) {
      flush();
    } else if (s.starts_with("#~")) {
      if (!s.substr(2).starts_with('|')) {
        body(skip_blanks(s.substr(2)), true);
      }
    } else if (s.front() == '#') {
      comment(s);
    } else {
      body(s, false);
    }
  }

  void comment(std::string_view s) {
    const char kind = s.size() > 1 ? s[1] : '\0';
    if (kind == '|') {
      return;
    }
    if (current_.has_str) {
      flush();
    } else if (current_.has_context || current_.has_id) {
      throw PoError(line_, "comment inside a message");
    }
    start();
    const bool marked = kind == '.' || kind == ':' || kind == ',';
    std::string_view rest = s.substr(marked ? 2 : 1);
    if (rest.starts_with(' ')) {
      rest.remove_prefix(1);
    }
    PoEntry& entry = current_.entry;
    if (kind == '.') {
      entry.extracted.emplace_back(rest);
    } else if (kind == ':') {
      for (std::string& word : split_words(rest, false)) {
        entry.references.push_back(std::move(word));
      }
    } else if (kind == ',') {
      for (std::string& word : split_words(rest, true)) {
        entry.flags.push_back(std::move(word));
      }
    } else {
      entry.comments.emplace_back(rest);
    }
  }

  void body(std::string_view s, bool obsolete) {
    if (s.starts_with('"')) {
      if (field_ == Field::kNone) {
        throw PoError(line_, "a string with no msgctxt, msgid or msgstr before it");
      }
      string_of(field_) += parse_quoted(s, line_);
      return;
    }
    const std::size_t word_end = std::min(s.find_first_of(" \t\""), s.size());
    const std::string_view keyword = s.substr(0, word_end);
    const std::string_view rest = skip_blanks(s.substr(word_end));
    if (!rest.starts_with('"')) {
      throw PoError(line_, "expected a quoted string after '" + std::string{keyword} + "'");
    }
    if ((keyword == "msgctxt" || keyword == "msgid") && current_.has_str) {
      flush();
    }
    start();
    PoEntry& entry = current_.entry;
    if (keyword == "msgctxt" && !current_.has_context && !current_.has_id) {
      current_.has_context = true;
      field_ = Field::kContext;
    } else if (keyword == "msgid" && !current_.has_id) {
      current_.has_id = true;
      entry.line = line_;
      field_ = Field::kId;
    } else if (keyword == "msgstr" && current_.has_id && !current_.has_str) {
      current_.has_str = true;
      field_ = Field::kStr;
    } else {
      throw PoError(line_, "'" + std::string{keyword} + "' is unknown or out of place");
    }
    entry.obsolete = entry.obsolete || obsolete;
    string_of(field_) = parse_quoted(rest, line_);
  }

  std::string& string_of(Field field) {
    PoEntry& entry = current_.entry;
    return field == Field::kContext ? entry.context : field == Field::kId ? entry.id : entry.str;
  }

  void start() {
    if (!current_.started) {
      current_.started = true;
      current_.first_line = line_;
    }
  }

  void flush() {
    if (!current_.started) {
      return;
    }
    if (!current_.has_id) {
      throw PoError(current_.first_line, "comment without a message");
    }
    if (!current_.has_str) {
      throw PoError(current_.first_line, "msgid without msgstr");
    }
    file_.entries.push_back(std::move(current_.entry));
    current_ = {};
    field_ = Field::kNone;
  }

  struct Current {
    PoEntry entry{};
    bool started = false;
    bool has_context = false;
    bool has_id = false;
    bool has_str = false;
    std::uint32_t first_line = 0;
  };

  PoFile file_{};
  Current current_{};
  Field field_ = Field::kNone;
  std::uint32_t line_ = 0;
};

void append_escaped(std::string& out, std::string_view text) {
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c == '\r') {
      out += "\\r";
    } else if (byte < ' ' || c == kDelete) {
      out.push_back('\\');
      out.push_back(static_cast<char>('0' + (byte >> 6)));
      out.push_back(static_cast<char>('0' + ((byte >> 3) & 7)));
      out.push_back(static_cast<char>('0' + (byte & 7)));
    } else {
      out.push_back(c);
    }
  }
}

void append_string(std::string& out, std::string_view prefix, std::string_view keyword,
                   std::string_view value) {
  const std::size_t newline = value.find('\n');
  out += prefix;
  out += keyword;
  if (newline == std::string_view::npos || newline + 1 == value.size()) {
    out += " \"";
    append_escaped(out, value);
    out += "\"\n";
    return;
  }
  out += " \"\"\n";
  while (!value.empty()) {
    const std::size_t end = value.find('\n') == std::string_view::npos ? value.size()
                                                                       : value.find('\n') + 1;
    out += prefix;
    out += '"';
    append_escaped(out, value.substr(0, end));
    out += "\"\n";
    value.remove_prefix(end);
  }
}

void append_comments(std::string& out, std::string_view marker,
                     const std::vector<std::string>& lines) {
  for (const std::string& line : lines) {
    out += marker;
    if (!line.empty()) {
      out += ' ';
      out += line;
    }
    out += '\n';
  }
}

}  // namespace

bool PoEntry::fuzzy() const { return std::ranges::find(flags, kFuzzy) != flags.end(); }

void PoEntry::set_fuzzy(bool fuzzy) {
  if (!fuzzy) {
    std::erase(flags, std::string{kFuzzy});
  } else if (!this->fuzzy()) {
    flags.emplace_back(kFuzzy);
  }
}

bool operator==(const PoEntry& a, const PoEntry& b) {
  return a.comments == b.comments && a.extracted == b.extracted && a.references == b.references &&
         a.flags == b.flags && a.context == b.context && a.id == b.id && a.str == b.str &&
         a.obsolete == b.obsolete;
}

PoFile parse_po(std::string_view text) { return Parser{}.run(text); }

std::string write_po(const PoFile& file) {
  std::string out;
  for (std::size_t i = 0; i < file.entries.size(); ++i) {
    const PoEntry& entry = file.entries[i];
    if (i > 0) {
      out += '\n';
    }
    append_comments(out, "#", entry.comments);
    append_comments(out, "#.", entry.extracted);
    if (!entry.references.empty()) {
      out += "#:";
      for (const std::string& reference : entry.references) {
        out += ' ' + reference;
      }
      out += '\n';
    }
    if (!entry.flags.empty()) {
      out += "#,";
      for (std::size_t f = 0; f < entry.flags.size(); ++f) {
        out += (f == 0 ? " " : ", ") + entry.flags[f];
      }
      out += '\n';
    }
    const std::string_view prefix = entry.obsolete ? "#~ " : "";
    if (!entry.context.empty()) {
      append_string(out, prefix, "msgctxt", entry.context);
    }
    append_string(out, prefix, "msgid", entry.id);
    append_string(out, prefix, "msgstr", entry.str);
  }
  return out;
}

}  // namespace vnmaki
