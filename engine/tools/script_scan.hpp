#pragma once

// A small Lua tokenizer and a scan for the engine's story calls (docs/SPEC.md section 18). It
// never runs a script, never throws, and accepts any bytes: a script with a syntax error gives the
// tokens up to that point and the best guess after it.

#include <cstddef>
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
  /// The bytes of the token in the source, as written (a string has its quotes or brackets). Two
  /// tokens are equal when kind, text and line are: the span says where a token came from.
  std::size_t begin = 0;
  std::size_t end = 0;

  friend bool operator==(const LuaToken& a, const LuaToken& b) {
    return a.kind == b.kind && a.text == b.text && a.line == b.line;
  }
};

/// Splits Lua 5.4 source into tokens. Comments (`--` and `--[==[ ]==]`) and white space give no
/// token. Strings are short (with escapes) or long (`[==[ ]==]`). A string or a long bracket that
/// is not closed runs to the end of the line (short string) or of the source. A line ends at LF,
/// CR, CRLF or LFCR. A byte that starts no other token is a one-byte symbol. A short string has its
/// escapes decoded, except `\u{...}`, which stays as written.
[[nodiscard]] std::vector<LuaToken> tokenize_lua(std::string_view source);

enum class ScanCallKind : std::uint8_t {
  kLabel,
  kJump,
  kCall,
  kRet,
  kMenu,
  kSay,
  kShow,
  kNarrate,
  kScene,
  kMusic,
  kSound,
  kVoice,
  kMovie,
};

/// One positional value of a table, as in `menu { "a", "b" }`. A `name = value` is not one.
struct ScanElement {
  /// The text when the value is one string literal, else nothing.
  std::optional<std::string> text{};
  std::uint32_t line = 0;  ///< The line of the first token of the value.

  friend bool operator==(const ScanElement&, const ScanElement&) = default;
};

/// A call of `label`, `jump`, `call`, `ret`, `menu`, `say`, `show`, `narrate`, `scene`, `music`,
/// `sound`, `voice` or `movie` in the body of a label,
/// written as `name(...)`, `name "text"` or `name { ... }`. A call after `.` or `:` (a method) is
/// not one.
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
  /// The keys written as `name =` at the top level of a table argument, in source order.
  std::vector<std::string> option_keys{};
  /// `say` only: the first argument as a string literal's text, or as a name (`eileen`,
  /// `chars.eileen`). Empty for `nil` and for any other value.
  std::string speaker{};
  /// `menu` only: the positional values of its table argument (the options). Nothing when the
  /// argument is not a table written in the call.
  std::optional<std::vector<ScanElement>> table{};

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

/// One call of `screen "name" { ... }` or `screen("name", function(args) ... end)`.
struct ScanScreen {
  std::optional<std::string> name{};  ///< Nothing when the name is computed.
  std::uint32_t line = 0;
  /// The first positional value of each `text { ... }` and `button { ... }` in the call, in source
  /// order: the string a player sees, or a computed value (no text).
  std::vector<ScanElement> texts{};

  friend bool operator==(const ScanScreen&, const ScanScreen&) = default;
};

/// The screens that the tokens define, in source order. A `text` or `button` after `.` or `:` is
/// not a node.
[[nodiscard]] std::vector<ScanScreen> scan_screen_tokens(std::span<const LuaToken> tokens);

/// `scan_screen_tokens(tokenize_lua(source))`.
[[nodiscard]] std::vector<ScanScreen> scan_screens(std::string_view source);

enum class ScanValueKind : std::uint8_t {
  kString,    ///< One string literal.
  kNumber,    ///< One number literal, with its minus sign if it has one.
  kBoolean,   ///< `true` or `false`.
  kComputed,  ///< Anything else, or a value that a later field of the same name replaces.
};

/// One field of a screen node table: `gap = 12`, `align = "center"`, or the string of
/// `text { "Hi" }` (its key is empty).
struct ScanField {
  std::string key{};
  ScanValueKind kind = ScanValueKind::kComputed;
  /// A string: its decoded content. A number: the text as written. A boolean: "true" or "false".
  /// Computed: empty.
  std::string text{};
  /// The bytes of the value in the source, as written (a string has its quotes). For a computed
  /// value: from its first token to its last.
  std::size_t begin = 0;
  std::size_t end = 0;
  std::uint32_t line = 0;

  friend bool operator==(const ScanField&, const ScanField&) = default;
};

/// A node written in a screen: `box { ... }`, `button { "Save", returns = "save" }`, or
/// `text "Hi"`.
struct ScanNode {
  /// `box`, `hbox`, `vbox`, `text`, `image`, `button`, `toggle`, `slider`, `bar`, `grid` or
  /// `input`.
  std::string kind{};
  std::uint32_t line = 0;
  /// The table has an entry that is neither a field nor a node written in it (a call, a name, a
  /// `for`). The children at run time need not match `children`, so none is mapped.
  bool dynamic = false;
  std::vector<ScanField> fields{};
  std::vector<ScanNode> children{};

  friend bool operator==(const ScanNode&, const ScanNode&) = default;
};

/// A screen written as a table: `screen "name" { ... }` or `screen("name", { ... })`.
struct ScanScreenTree {
  std::optional<std::string> name{};
  std::uint32_t line = 0;
  /// The root node. A table with one node is that node (the engine ignores its other fields). Any
  /// other table is the box that the engine wraps it in: kind `box`, the fields of the table
  /// (`modal`, ...), and `dynamic` when the table has an entry that is not a node.
  std::optional<ScanNode> root{};

  friend bool operator==(const ScanScreenTree&, const ScanScreenTree&) = default;
};

/// The screens that the tokens define in table form, in source order, with the nodes and the
/// source span of each field (the screen editor, docs/SPEC.md section 18). Not recorded: the
/// function form `screen("name", function(args) ... end)`, whose nodes depend on `args`, and a
/// screen with a computed name. The tokens must come from tokenize_lua, which gives the spans.
[[nodiscard]] std::vector<ScanScreenTree> scan_screen_trees(std::span<const LuaToken> tokens);

/// `scan_screen_trees(tokenize_lua(source))`.
[[nodiscard]] std::vector<ScanScreenTree> scan_screen_trees(std::string_view source);

}  // namespace vnmaki
