#include "engine/tools/script_scan.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vnmaki {

namespace {

constexpr int kHexBase = 16;
constexpr int kDecimalBase = 10;
constexpr int kHexEscapeDigits = 2;
constexpr int kDecimalEscapeDigits = 3;
constexpr unsigned kByteMask = 0xFFU;
constexpr std::size_t kLongBracketOpenLength = 2;  // the two "[" around the "=" signs
constexpr std::size_t kCommentOpenLength = 2;      // "--"

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_name_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

bool is_name_char(char c) { return is_name_start(c) || is_digit(c); }

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f'; }

// The value of a hexadecimal digit, or -1.
int hex_value(char c) {
  if (is_digit(c)) {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + kDecimalBase;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + kDecimalBase;
  }
  return -1;
}

std::optional<char> simple_escape(char escape) {
  switch (escape) {
    case 'a':
      return '\a';
    case 'b':
      return '\b';
    case 'f':
      return '\f';
    case 'n':
      return '\n';
    case 'r':
      return '\r';
    case 't':
      return '\t';
    case 'v':
      return '\v';
    case '\\':
    case '"':
    case '\'':
      return escape;
    default:
      return std::nullopt;
  }
}

class Lexer {
 public:
  explicit Lexer(std::string_view source) : source_(source) {}

  std::vector<LuaToken> run() {
    while (pos_ < source_.size()) {
      token_begin_ = pos_;
      const char c = source_[pos_];
      const std::optional<std::size_t> level = c == '[' ? long_bracket_level() : std::nullopt;
      if (take_newline()) {
        continue;
      }
      if (is_blank(c)) {
        ++pos_;
      } else if (c == '-' && peek(1) == '-') {
        skip_comment();
      } else if (level) {
        const std::uint32_t line = line_;
        add(LuaTokenKind::kString, read_long(*level), line);
      } else if (c == '"' || c == '\'') {
        const std::uint32_t line = line_;
        add(LuaTokenKind::kString, read_short_string(c), line);
      } else if (is_name_start(c)) {
        add(LuaTokenKind::kName, read_while_name(), line_);
      } else if (is_digit(c) || (c == '.' && is_digit(peek(1)))) {
        add(LuaTokenKind::kNumber, read_number(), line_);
      } else {
        ++pos_;
        add(LuaTokenKind::kSymbol, std::string(1, c), line_);
      }
    }
    return std::move(tokens_);
  }

 private:
  [[nodiscard]] char peek(std::size_t ahead) const {
    return pos_ + ahead < source_.size() ? source_[pos_ + ahead] : '\0';
  }

  void add(LuaTokenKind kind, std::string text, std::uint32_t line) {
    tokens_.push_back(LuaToken{
        .kind = kind, .text = std::move(text), .line = line, .begin = token_begin_, .end = pos_});
  }

  // Consumes one line break (LF, CR, CRLF or LFCR) and counts it.
  bool take_newline() {
    const char c = pos_ < source_.size() ? source_[pos_] : '\0';
    if (c != '\n' && c != '\r') {
      return false;
    }
    ++pos_;
    if (pos_ < source_.size() && (source_[pos_] == '\n' || source_[pos_] == '\r') &&
        source_[pos_] != c) {
      ++pos_;
    }
    ++line_;
    return true;
  }

  // At a "[": the number of "=" signs when "[=*[" starts here.
  [[nodiscard]] std::optional<std::size_t> long_bracket_level() const {
    std::size_t level = 0;
    while (peek(1 + level) == '=') {
      ++level;
    }
    if (peek(1 + level) == '[') {
      return level;
    }
    return std::nullopt;
  }

  // At the "[" of a long bracket. Returns the content with every line break as LF.
  std::string read_long(std::size_t level) {
    pos_ += level + kLongBracketOpenLength;
    take_newline();  // a line break right after the opening bracket is not content
    std::string text;
    while (pos_ < source_.size()) {
      if (source_[pos_] == ']' && closes_long(level)) {
        pos_ += level + kLongBracketOpenLength;
        break;
      }
      if (take_newline()) {
        text.push_back('\n');
      } else {
        text.push_back(source_[pos_]);
        ++pos_;
      }
    }
    return text;
  }

  // At a "]": whether "]=*]" with `level` "=" signs starts here.
  [[nodiscard]] bool closes_long(std::size_t level) const {
    for (std::size_t i = 1; i <= level; ++i) {
      if (peek(i) != '=') {
        return false;
      }
    }
    return peek(level + 1) == ']';
  }

  void skip_comment() {
    pos_ += kCommentOpenLength;
    const std::optional<std::size_t> level = peek(0) == '[' ? long_bracket_level() : std::nullopt;
    if (level) {
      read_long(*level);
      return;
    }
    while (pos_ < source_.size() && source_[pos_] != '\n' && source_[pos_] != '\r') {
      ++pos_;
    }
  }

  // At the opening quote. A string that is not closed ends at the line break.
  std::string read_short_string(char quote) {
    ++pos_;
    std::string text;
    while (pos_ < source_.size()) {
      const char c = source_[pos_];
      if (c == quote) {
        ++pos_;
        break;
      }
      if (c == '\n' || c == '\r') {
        break;
      }
      ++pos_;
      if (c != '\\') {
        text.push_back(c);
      } else if (pos_ < source_.size()) {
        read_escape(text);
      }
    }
    return text;
  }

  // After a backslash. `\u{...}` stays as written: no code needs it decoded.
  void read_escape(std::string& text) {
    const char escape = source_[pos_];
    if (const std::optional<char> simple = simple_escape(escape)) {
      text.push_back(*simple);
      ++pos_;
    } else if (take_newline()) {
      text.push_back('\n');
    } else if (escape == 'x') {
      ++pos_;
      text.push_back(static_cast<char>(read_number_escape(kHexBase, kHexEscapeDigits)));
    } else if (escape == 'z') {
      ++pos_;
      while (pos_ < source_.size()) {
        if (is_blank(source_[pos_])) {
          ++pos_;
        } else if (!take_newline()) {
          break;
        }
      }
    } else if (is_digit(escape)) {
      text.push_back(static_cast<char>(read_number_escape(kDecimalBase, kDecimalEscapeDigits)));
    } else if (escape == 'u') {
      text += "\\u";
      ++pos_;
    } else {
      text.push_back(escape);
      ++pos_;
    }
  }

  // Up to `digits` digits of `base`. Returns the low byte of their value.
  unsigned read_number_escape(int base, int digits) {
    unsigned value = 0;
    for (int i = 0; i < digits && pos_ < source_.size(); ++i) {
      const int digit = hex_value(source_[pos_]);
      if (digit < 0 || digit >= base) {
        break;
      }
      value = value * static_cast<unsigned>(base) + static_cast<unsigned>(digit);
      ++pos_;
    }
    return value & kByteMask;
  }

  std::string read_while_name() {
    const std::size_t begin = pos_;
    while (pos_ < source_.size() && is_name_char(source_[pos_])) {
      ++pos_;
    }
    return std::string{source_.substr(begin, pos_ - begin)};
  }

  // A number is a digit (or a dot and a digit), then letters, digits and dots, and a sign right
  // after an exponent letter. The text is not checked: nothing here needs the value.
  std::string read_number() {
    const std::size_t begin = pos_;
    const bool hex = source_[pos_] == '0' && (peek(1) == 'x' || peek(1) == 'X');
    const char exponent = hex ? 'p' : 'e';
    while (pos_ < source_.size()) {
      const char c = source_[pos_];
      const bool sign_after_exponent =
          (c == '+' || c == '-') && pos_ > begin && lower(source_[pos_ - 1]) == exponent;
      if (!is_name_char(c) && c != '.' && !sign_after_exponent) {
        break;
      }
      ++pos_;
    }
    return std::string{source_.substr(begin, pos_ - begin)};
  }

  std::string_view source_;
  std::size_t pos_ = 0;
  std::size_t token_begin_ = 0;  // where the token that add() gets starts
  std::uint32_t line_ = 1;
  std::vector<LuaToken> tokens_;
};

bool is_symbol(const LuaToken& token, char c) {
  return token.kind == LuaTokenKind::kSymbol && token.text.size() == 1 && token.text.front() == c;
}

bool is_name(const LuaToken& token, std::string_view name) {
  return token.kind == LuaTokenKind::kName && token.text == name;
}

std::optional<ScanCallKind> call_kind(std::string_view name) {
  struct Entry {
    std::string_view name{};
    ScanCallKind kind = ScanCallKind::kLabel;
  };
  static constexpr std::array<Entry, 14> kEntries{{
      {.name = "label", .kind = ScanCallKind::kLabel},
      {.name = "jump", .kind = ScanCallKind::kJump},
      {.name = "call", .kind = ScanCallKind::kCall},
      {.name = "ret", .kind = ScanCallKind::kRet},
      {.name = "menu", .kind = ScanCallKind::kMenu},
      {.name = "say", .kind = ScanCallKind::kSay},
      {.name = "show", .kind = ScanCallKind::kShow},
      {.name = "narrate", .kind = ScanCallKind::kNarrate},
      {.name = "scene", .kind = ScanCallKind::kScene},
      {.name = "music", .kind = ScanCallKind::kMusic},
      {.name = "sound", .kind = ScanCallKind::kSound},
      {.name = "voice", .kind = ScanCallKind::kVoice},
      {.name = "movie", .kind = ScanCallKind::kMovie},
      {.name = "ambient", .kind = ScanCallKind::kAmbient},
  }};
  for (const Entry& entry : kEntries) {
    if (entry.name == name) {
      return entry.kind;
    }
  }
  return std::nullopt;
}

struct Range {
  std::size_t begin = 0;
  std::size_t end = 0;
};

// The token ranges of the arguments of the call whose "(" is tokens[open].
std::vector<Range> argument_ranges(std::span<const LuaToken> tokens, std::size_t open) {
  std::vector<Range> ranges;
  std::size_t nesting = 0;
  std::size_t begin = open + 1;
  bool closed = false;
  for (std::size_t i = begin; i < tokens.size() && !closed; ++i) {
    if (tokens[i].kind != LuaTokenKind::kSymbol || tokens[i].text.empty()) {
      continue;
    }
    const char c = tokens[i].text.front();
    if (c == '(' || c == '[' || c == '{') {
      ++nesting;
    } else if (c == ')' || c == ']' || c == '}') {
      if (nesting > 0) {
        --nesting;
      } else if (c == ')') {
        ranges.push_back({.begin = begin, .end = i});
        closed = true;
      }
    } else if (c == ',' && nesting == 0) {
      ranges.push_back({.begin = begin, .end = i});
      begin = i + 1;
    }
  }
  if (!closed) {
    ranges.push_back({.begin = begin, .end = tokens.size()});
  }
  if (ranges.size() == 1 && ranges.front().begin == ranges.front().end) {
    ranges.clear();  // "()"
  }
  return ranges;
}

std::optional<std::string> literal(std::span<const LuaToken> tokens, const Range& range) {
  if (range.end == range.begin + 1 && tokens[range.begin].kind == LuaTokenKind::kString) {
    return tokens[range.begin].text;
  }
  return std::nullopt;
}

// `ending = true` among the tokens of the range.
bool has_ending(std::span<const LuaToken> tokens, const Range& range) {
  for (std::size_t i = range.begin; i + 2 < range.end; ++i) {
    if (is_name(tokens[i], "ending") && is_symbol(tokens[i + 1], '=') &&
        is_name(tokens[i + 2], "true")) {
      return true;
    }
  }
  return false;
}

// The `name =` keys at the top level of the table that the range starts with.
void add_table_keys(std::span<const LuaToken> tokens, const Range& range,
                    std::vector<std::string>& keys) {
  if (range.begin >= range.end || !is_symbol(tokens[range.begin], '{')) {
    return;
  }
  std::size_t nesting = 0;
  for (std::size_t i = range.begin; i < range.end; ++i) {
    const LuaToken& token = tokens[i];
    if (token.kind == LuaTokenKind::kSymbol && token.text.size() == 1) {
      const char c = token.text.front();
      if (c == '{' || c == '(' || c == '[') {
        ++nesting;
      } else if ((c == '}' || c == ')' || c == ']') && nesting > 0) {
        --nesting;
      }
    } else if (token.kind == LuaTokenKind::kName && nesting == 1 && i + 2 < range.end &&
               (is_symbol(tokens[i - 1], '{') || is_symbol(tokens[i - 1], ',')) &&
               is_symbol(tokens[i + 1], '=') && !is_symbol(tokens[i + 2], '=')) {
      keys.push_back(token.text);
    }
  }
}

// The index after the bracket that closes the one at tokens[open], or tokens.size().
std::size_t matching_end(std::span<const LuaToken> tokens, std::size_t open) {
  std::size_t nesting = 0;
  for (std::size_t i = open; i < tokens.size(); ++i) {
    if (tokens[i].kind != LuaTokenKind::kSymbol || tokens[i].text.size() != 1) {
      continue;
    }
    const char c = tokens[i].text.front();
    if (c == '(' || c == '[' || c == '{') {
      ++nesting;
    } else if ((c == ')' || c == ']' || c == '}') && nesting > 0 && --nesting == 0) {
      return i + 1;
    }
  }
  return tokens.size();
}

// `name =` or `[` at the start of the range: a keyed value of a table.
bool is_keyed(std::span<const LuaToken> tokens, const Range& range) {
  if (is_symbol(tokens[range.begin], '[')) {
    return true;
  }
  return range.end >= range.begin + 3 && tokens[range.begin].kind == LuaTokenKind::kName &&
         is_symbol(tokens[range.begin + 1], '=') && !is_symbol(tokens[range.begin + 2], '=');
}

// The positional values of the table that starts at tokens[open] (a "{").
// ponytail: a comma inside a `function ... end` in a table splits it wrongly; real tables hold
// plain values, and `ScanScreen` reads only the first one.
std::vector<ScanElement> table_elements(std::span<const LuaToken> tokens, std::size_t open) {
  std::vector<ScanElement> elements;
  const std::size_t end = matching_end(tokens, open);
  const std::size_t last = end > open + 1 && is_symbol(tokens[end - 1], '}') ? end - 1 : end;
  std::size_t nesting = 0;
  std::size_t begin = open + 1;
  const auto add = [&](std::size_t stop) {
    const Range range{.begin = begin, .end = stop};
    if (range.begin < range.end && !is_keyed(tokens, range)) {
      elements.push_back({.text = literal(tokens, range), .line = tokens[range.begin].line});
    }
  };
  for (std::size_t i = open + 1; i < last; ++i) {
    if (tokens[i].kind != LuaTokenKind::kSymbol || tokens[i].text.size() != 1) {
      continue;
    }
    const char c = tokens[i].text.front();
    if (c == '(' || c == '[' || c == '{') {
      ++nesting;
    } else if ((c == ')' || c == ']' || c == '}') && nesting > 0) {
      --nesting;
    } else if ((c == ',' || c == ';') && nesting == 0) {
      add(i);
      begin = i + 1;
    }
  }
  add(last);
  return elements;
}

// A name or a dotted name (`eileen`, `chars.eileen`) as written, else nothing.
std::string dotted_name(std::span<const LuaToken> tokens, const Range& range) {
  std::string name;
  for (std::size_t i = range.begin; i < range.end; ++i) {
    const bool expect_name = (i - range.begin) % 2 == 0;
    if (expect_name ? tokens[i].kind != LuaTokenKind::kName : !is_symbol(tokens[i], '.')) {
      return {};
    }
    name += tokens[i].text;
  }
  return (range.end - range.begin) % 2 == 1 && name != "nil" ? name : std::string{};
}

bool is_member_use(std::span<const LuaToken> tokens, std::size_t i) {
  return i > 0 && (is_symbol(tokens[i - 1], '.') || is_symbol(tokens[i - 1], ':') ||
                   is_name(tokens[i - 1], "function"));
}

bool starts_block(std::string_view name) {
  return name == "function" || name == "if" || name == "do" || name == "repeat";
}

}  // namespace

std::vector<LuaToken> tokenize_lua(std::string_view source) { return Lexer{source}.run(); }

std::vector<ScanLabel> scan_tokens(std::span<const LuaToken> tokens) {
  std::vector<ScanLabel> labels;
  std::optional<ScanLabel> open;  // the label whose function is being read
  std::uint32_t depth = 0;
  std::uint32_t body_depth = 0;               // `depth` outside the function of `open`
  std::optional<std::size_t> function_token;  // where the function of `pending` starts
  std::optional<ScanLabel> pending;

  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const LuaToken& token = tokens[i];
    if (token.kind != LuaTokenKind::kName) {
      continue;
    }
    if (starts_block(token.text)) {
      if (function_token == i) {
        open = std::move(pending);
        pending.reset();
        body_depth = depth;
        function_token.reset();
      }
      ++depth;
      continue;
    }
    if (token.text == "end" || token.text == "until") {
      if (depth > 0) {
        --depth;
      }
      if (open && depth == body_depth) {
        open->end_line = token.line;
        labels.push_back(std::move(*open));
        open.reset();
      }
      continue;
    }
    const std::optional<ScanCallKind> kind = call_kind(token.text);
    if (!kind || is_member_use(tokens, i) || i + 1 >= tokens.size()) {
      continue;
    }
    const bool is_label = *kind == ScanCallKind::kLabel;
    const LuaToken& next = tokens[i + 1];
    ScanCall call{.kind = *kind, .line = token.line, .depth = open ? depth - body_depth - 1 : 0};
    std::vector<Range> ranges;
    if (is_symbol(next, '(')) {
      ranges = argument_ranges(tokens, i + 1);
      for (const Range& range : ranges) {
        call.arguments.push_back(literal(tokens, range));
        add_table_keys(tokens, range, call.option_keys);
      }
      if (*kind == ScanCallKind::kSay && !ranges.empty()) {
        call.speaker = call.arguments.front().value_or(dotted_name(tokens, ranges.front()));
      }
      if (*kind == ScanCallKind::kMenu && !ranges.empty() &&
          is_symbol(tokens[ranges.front().begin], '{')) {
        call.table = table_elements(tokens, ranges.front().begin);
      }
    } else if (!is_label && next.kind == LuaTokenKind::kString) {
      call.arguments.emplace_back(next.text);
    } else if (!is_label && is_symbol(next, '{')) {
      call.arguments.emplace_back();
      if (*kind == ScanCallKind::kMenu) {
        call.table = table_elements(tokens, i + 1);
      }
    } else {
      continue;
    }
    if (!is_label) {
      if (open) {
        open->calls.push_back(std::move(call));
      }
    } else if (!open) {
      ScanLabel label{.line = token.line, .end_line = token.line};
      if (!call.arguments.empty()) {
        label.name = call.arguments.front();
      }
      if (ranges.size() >= 2 && ranges[1].begin < ranges[1].end &&
          is_name(tokens[ranges[1].begin], "function")) {
        label.ending = ranges.size() >= 3 && has_ending(tokens, ranges[2]);
        function_token = ranges[1].begin;
        pending = std::move(label);
      } else {
        labels.push_back(std::move(label));
      }
    }
  }
  if (open) {
    open->end_line = tokens.back().line;
    labels.push_back(std::move(*open));
  }
  return labels;
}

std::vector<ScanLabel> scan_labels(std::string_view source) {
  const std::vector<LuaToken> tokens = tokenize_lua(source);
  return scan_tokens(tokens);
}

std::vector<ScanScreen> scan_screen_tokens(std::span<const LuaToken> tokens) {
  std::vector<ScanScreen> screens;
  for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
    if (!is_name(tokens[i], "screen") || is_member_use(tokens, i)) {
      continue;
    }
    ScanScreen screen{.line = tokens[i].line};
    std::size_t end = 0;  // after the last token of the call
    if (is_symbol(tokens[i + 1], '(')) {
      const std::vector<Range> ranges = argument_ranges(tokens, i + 1);
      if (ranges.empty()) {
        continue;
      }
      screen.name = literal(tokens, ranges.front());
      end = ranges.back().end;
    } else if (tokens[i + 1].kind == LuaTokenKind::kString && i + 2 < tokens.size() &&
               is_symbol(tokens[i + 2], '{')) {
      screen.name = tokens[i + 1].text;
      end = matching_end(tokens, i + 2);
    } else {
      continue;
    }
    for (std::size_t j = i + 2; j + 1 < end; ++j) {
      const bool node = is_name(tokens[j], "text") || is_name(tokens[j], "button");
      if (node && !is_member_use(tokens, j) && is_symbol(tokens[j + 1], '{')) {
        const std::vector<ScanElement> values = table_elements(tokens, j + 1);
        if (!values.empty()) {
          screen.texts.push_back(values.front());
        }
      }
    }
    screens.push_back(std::move(screen));
  }
  return screens;
}

std::vector<ScanScreen> scan_screens(std::string_view source) {
  const std::vector<LuaToken> tokens = tokenize_lua(source);
  return scan_screen_tokens(tokens);
}

namespace {

// The nesting of node tables that the screen scan follows; deeper ones count as computed.
constexpr std::size_t kMaxNodeDepth = 64;

constexpr std::array<std::string_view, 11> kNodeKinds{
    "box", "hbox", "vbox", "text", "image", "button", "toggle", "slider", "bar", "grid", "input"};

bool is_node_kind(std::string_view name) {
  return std::ranges::find(kNodeKinds, name) != kNodeKinds.end();
}

// The kinds that take a string as a positional item (`node_takes_text` of the Lua prelude).
bool takes_text(std::string_view kind) {
  return kind == "text" || kind == "button" || kind == "image" || kind == "toggle" ||
         kind == "input";
}

// The value of a field, from the tokens of the range (not empty).
ScanField make_field(std::span<const LuaToken> tokens, std::string key, const Range& value) {
  const LuaToken& first = tokens[value.begin];
  ScanField field{.key = std::move(key),
                  .begin = first.begin,
                  .end = tokens[value.end - 1].end,
                  .line = first.line};
  const std::size_t count = value.end - value.begin;
  if (count == 1 && first.kind == LuaTokenKind::kString) {
    field.kind = ScanValueKind::kString;
    field.text = first.text;
  } else if (count == 1 && first.kind == LuaTokenKind::kNumber) {
    field.kind = ScanValueKind::kNumber;
    field.text = first.text;
  } else if (count == 2 && is_symbol(first, '-') &&
             tokens[value.begin + 1].kind == LuaTokenKind::kNumber) {
    field.kind = ScanValueKind::kNumber;
    field.text = "-" + tokens[value.begin + 1].text;
  } else if (count == 1 && (is_name(first, "true") || is_name(first, "false"))) {
    field.kind = ScanValueKind::kBoolean;
    field.text = first.text;
  }
  return field;
}

// The entries of the table that starts at tokens[open] (a "{"), split at the commas and
// semicolons of its top level. Same ponytail as table_elements: a comma in a `function ... end`
// splits an entry.
std::vector<Range> table_entries(std::span<const LuaToken> tokens, std::size_t open) {
  std::vector<Range> entries;
  const std::size_t end = matching_end(tokens, open);
  const std::size_t last = end > open + 1 && is_symbol(tokens[end - 1], '}') ? end - 1 : end;
  std::size_t nesting = 0;
  std::size_t begin = open + 1;
  const auto add = [&](std::size_t stop) {
    if (begin < stop) {
      entries.push_back({.begin = begin, .end = stop});
    }
  };
  for (std::size_t i = open + 1; i < last; ++i) {
    if (tokens[i].kind != LuaTokenKind::kSymbol || tokens[i].text.size() != 1) {
      continue;
    }
    const char c = tokens[i].text.front();
    if (c == '(' || c == '[' || c == '{') {
      ++nesting;
    } else if ((c == ')' || c == ']' || c == '}') && nesting > 0) {
      --nesting;
    } else if ((c == ',' || c == ';') && nesting == 0) {
      add(i);
      begin = i + 1;
    }
  }
  add(last);
  return entries;
}

void add_field(ScanNode& node, ScanField field) {
  if (!field.key.empty()) {
    for (ScanField& earlier : node.fields) {
      if (earlier.key == field.key) {  // the later one wins at run time
        earlier.kind = ScanValueKind::kComputed;
        earlier.text.clear();
      }
    }
  }
  node.fields.push_back(std::move(field));
}

// The node whose table starts at tokens[open] (a "{").
ScanNode parse_node_table(std::span<const LuaToken> tokens, std::string kind, std::uint32_t line,
                          std::size_t open, std::size_t depth) {
  ScanNode node{.kind = std::move(kind), .line = line};
  bool has_text = false;
  for (const Range& entry : table_entries(tokens, open)) {
    const LuaToken& first = tokens[entry.begin];
    const bool has_next = entry.end > entry.begin + 1;
    if (first.kind == LuaTokenKind::kName && entry.end > entry.begin + 2 &&
        is_symbol(tokens[entry.begin + 1], '=') && !is_symbol(tokens[entry.begin + 2], '=')) {
      add_field(node, make_field(tokens, first.text, {.begin = entry.begin + 2, .end = entry.end}));
    } else if (first.kind == LuaTokenKind::kString && !has_next && takes_text(node.kind) &&
               !has_text) {
      has_text = true;
      add_field(node, make_field(tokens, "", entry));
    } else if (first.kind == LuaTokenKind::kName && is_node_kind(first.text) && has_next &&
               depth < kMaxNodeDepth && is_symbol(tokens[entry.begin + 1], '{') &&
               matching_end(tokens, entry.begin + 1) == entry.end) {
      node.children.push_back(
          parse_node_table(tokens, first.text, first.line, entry.begin + 1, depth + 1));
    } else if (first.kind == LuaTokenKind::kName && takes_text(first.text) && has_next &&
               entry.end == entry.begin + 2 &&
               tokens[entry.begin + 1].kind == LuaTokenKind::kString) {  // text "Hi"
      ScanNode child{.kind = first.text, .line = first.line};
      add_field(child, make_field(tokens, "", {.begin = entry.begin + 1, .end = entry.end}));
      node.children.push_back(std::move(child));
    } else {
      node.dynamic = true;
    }
  }
  return node;
}

// The root that the prelude's screen_root makes of the table of a screen.
ScanNode screen_root_node(ScanNode table) {
  if (!table.dynamic && table.children.size() == 1) {
    return std::move(table.children.front());
  }
  return table;
}

}  // namespace

std::vector<ScanScreenTree> scan_screen_trees(std::span<const LuaToken> tokens) {
  std::vector<ScanScreenTree> screens;
  for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
    if (!is_name(tokens[i], "screen") || is_member_use(tokens, i)) {
      continue;
    }
    ScanScreenTree screen{.line = tokens[i].line};
    std::optional<std::size_t> table;  // the "{" of the table form
    if (is_symbol(tokens[i + 1], '(')) {
      const std::vector<Range> ranges = argument_ranges(tokens, i + 1);
      if (ranges.empty()) {
        continue;
      }
      screen.name = literal(tokens, ranges.front());
      if (ranges.size() >= 2 && ranges[1].begin < ranges[1].end &&
          is_symbol(tokens[ranges[1].begin], '{') &&
          matching_end(tokens, ranges[1].begin) == ranges[1].end) {
        table = ranges[1].begin;
      }
    } else if (tokens[i + 1].kind == LuaTokenKind::kString && i + 2 < tokens.size() &&
               is_symbol(tokens[i + 2], '{')) {
      screen.name = tokens[i + 1].text;
      table = i + 2;
    } else {
      continue;
    }
    if (screen.name && table) {
      screen.root = screen_root_node(parse_node_table(tokens, "box", screen.line, *table, 0));
    }
    screens.push_back(std::move(screen));
  }
  return screens;
}

std::vector<ScanScreenTree> scan_screen_trees(std::string_view source) {
  const std::vector<LuaToken> tokens = tokenize_lua(source);
  return scan_screen_trees(tokens);
}

}  // namespace vnmaki
