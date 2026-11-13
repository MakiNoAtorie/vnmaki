#pragma once

// A small Lua tokenizer and a scan for the engine's story calls (docs/SPEC.md section 18). It
// never runs a script, never throws, and accepts any bytes: a script with a syntax error gives the
// tokens up to that point and the best guess after it.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vnmaki {

enum class LuaTokenKind : std::uint8_t {
  kName,    ///< An identifier or a keyword.
  kNumber,  ///< The text as written.
  kString,  ///< `text` is the decoded content, without quotes or brackets.
  kSymbol,  ///< One byte: an operator, a bracket, a comma, ...
};

struct LuaToken {
  LuaTokenKind kind = LuaTokenKind::kName;
  std::string text{};
  std::uint32_t line = 1;  ///< From 1; for a string, the line where it starts.

  friend bool operator==(const LuaToken&, const LuaToken&) = default;
};

/// Splits Lua 5.4 source into tokens. Comments (`--` and `--[==[ ]==]`) and white space give no
/// token. Strings are short (with escapes) or long (`[==[ ]==]`). A string or a long bracket that
/// is not closed runs to the end of the line (short string) or of the source. A line ends at LF,
/// CR, CRLF or LFCR. A byte that starts no other token is a one-byte symbol.
[[nodiscard]] std::vector<LuaToken> tokenize_lua(std::string_view source);

enum class ScanCallKind : std::uint8_t { kLabel, kJump, kCall, kRet, kMenu, kSay, kShow };

/// A call of `label`, `jump`, `call`, `ret`, `menu`, `say` or `show` in the body of a label, written
/// as `name(...)`, `name "text"` or `name { ... }`. A call after `.` or `:` (a method) is not one.
struct ScanCall {
  ScanCallKind kind = ScanCallKind::kJump;
  std::uint32_t line = 0;
  /// Blocks (`if`, `do`, `repeat`, `function`) open inside the label function: 0 is directly in
  /// its body.
  std::uint32_t depth = 0;
  /// One entry for each top-level argument: the text when the argument is one string literal,
  /// else nothing (a computed value, a table). `name "text"` has one literal; `name { }` has one
  /// entry without text.
  std::vector<std::optional<std::string>> arguments{};

  friend bool operator==(const ScanCall&, const ScanCall&) = default;
};

/// One call of `label(name, function() ... end)` and the story calls in its function.
struct ScanLabel {
  std::optional<std::string> name{};  ///< Nothing when the name is computed.
  std::uint32_t line = 0;             ///< The line of the `label` call.
  std::uint32_t end_line = 0;         ///< The line of the `end` of its function (else `line`).
  /// The third argument is a table with `ending = true`: the story ends here on purpose.
  bool ending = false;
  /// In source order. Calls of `label` inside a label function belong to the outer label and are
  /// not recorded.
  std::vector<ScanCall> calls{};

  friend bool operator==(const ScanLabel&, const ScanLabel&) = default;
};

/// The labels that the tokens define, in source order. `jump`, `say` and so on outside a label
/// function are not recorded.
/// ponytail: finding the arguments of a call again for each call is quadratic for calls nested in
/// calls without end; real scripts do not nest.
[[nodiscard]] std::vector<ScanLabel> scan_tokens(std::span<const LuaToken> tokens);

/// `scan_tokens(tokenize_lua(source))`.
[[nodiscard]] std::vector<ScanLabel> scan_labels(std::string_view source);

}  // namespace vnmaki
