#pragma once

// What must hold when tokenize_lua and scan_tokens (engine/tools/script_scan.hpp) read any byte
// string. The fuzz target fuzz/script_scan_fuzzer.cpp and a unit test
// (tests/unit/script_scan_test.cpp) both run it, so the check itself is tested on compilers that
// build no fuzzers.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "engine/tools/script_scan.hpp"

namespace vnmaki::fuzz {

inline void require_scan(bool condition) {
  if (!condition) {
    std::abort();
  }
}

inline bool is_plain_name(std::string_view text) {
  if (text.empty() || (text.front() >= '0' && text.front() <= '9')) {
    return false;
  }
  for (const char c : text) {
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (!letter && c != '_' && (c < '0' || c > '9')) {
      return false;
    }
  }
  return true;
}

/// The tokens as source: a token on a later line than the one before follows after the line breaks
/// that make the difference, any other token follows after a space. A string is a short string with
/// each byte that is not printable ASCII (and `"` and `\`) as a three-digit escape.
inline std::string render_lua_tokens(const std::vector<LuaToken>& tokens) {
  constexpr std::size_t kEscapeDigits = 3;
  constexpr unsigned kDecimalBase = 10;
  std::string source;
  std::uint32_t line = 1;
  for (const LuaToken& token : tokens) {
    if (token.line > line) {
      source.append(token.line - line, '\n');
      line = token.line;
    } else if (!source.empty()) {
      source.push_back(' ');
    }
    if (token.kind != LuaTokenKind::kString) {
      source += token.text;
      continue;
    }
    source.push_back('"');
    for (const char c : token.text) {
      auto byte = static_cast<unsigned char>(c);
      if (byte >= ' ' && byte <= '~' && c != '"' && c != '\\') {
        source.push_back(c);
        continue;
      }
      std::string digits(kEscapeDigits, '0');
      for (std::size_t i = kEscapeDigits; i > 0; --i) {
        digits[i - 1] = static_cast<char>('0' + byte % kDecimalBase);
        byte = static_cast<unsigned char>(byte / kDecimalBase);
      }
      source += '\\' + digits;
    }
    source.push_back('"');
  }
  return source;
}

/// Calls std::abort() unless:
/// - no input makes the tokenizer or the scan fail;
/// - the lines of the tokens do not go down and are at most 1 + the count of LF and CR bytes;
/// - a name is a Lua identifier, a symbol is one byte, and the text of all tokens together is not
///   longer than the input;
/// - the tokens rendered as source (`render_lua_tokens`) tokenize to the same tokens, with the same
///   lines, and scan to the same labels (the scan reads tokens only);
/// - each label ends at or after its line, and its calls are in order, between those two lines.
inline void check_script_scan_properties(std::string_view source) {
  const std::vector<LuaToken> tokens = tokenize_lua(source);

  std::size_t line_breaks = 0;
  for (const char c : source) {
    line_breaks += c == '\n' || c == '\r' ? 1 : 0;
  }
  std::uint32_t line = 1;
  std::size_t text_size = 0;
  for (const LuaToken& token : tokens) {
    require_scan(token.line >= line && token.line <= 1 + line_breaks);
    line = token.line;
    text_size += token.text.size();
    if (token.kind == LuaTokenKind::kName) {
      require_scan(is_plain_name(token.text));
    } else if (token.kind == LuaTokenKind::kSymbol) {
      require_scan(token.text.size() == 1);
    } else if (token.kind == LuaTokenKind::kNumber) {
      require_scan(!token.text.empty());
    }
  }
  require_scan(text_size <= source.size());

  const std::string rendered = render_lua_tokens(tokens);
  require_scan(tokenize_lua(rendered) == tokens);

  const std::vector<ScanLabel> labels = scan_tokens(tokens);
  require_scan(scan_labels(rendered) == labels);
  for (const ScanLabel& label : labels) {
    require_scan(label.end_line >= label.line);
    std::uint32_t call_line = label.line;
    for (const ScanCall& call : label.calls) {
      require_scan(call.line >= call_line && call.line <= label.end_line);
      call_line = call.line;
    }
  }
}

}  // namespace vnmaki::fuzz
