#include "engine/vfs/path.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vnmaki {
namespace {

// The lead byte of a multi-byte UTF-8 sequence: (byte & mask) == tag. The
// bits outside the mask are the top bits of the code point.
struct Utf8Lead {
  std::uint8_t mask;
  std::uint8_t tag;
  std::size_t length;
  std::uint32_t min_code_point;  // Anything smaller is an overlong encoding.
};

constexpr std::uint8_t kAsciiEnd = 0x80;
constexpr std::array<Utf8Lead, 3> kUtf8Leads = {{
    {.mask = 0xE0, .tag = 0xC0, .length = 2, .min_code_point = 0x80},
    {.mask = 0xF0, .tag = 0xE0, .length = 3, .min_code_point = 0x800},
    {.mask = 0xF8, .tag = 0xF0, .length = 4, .min_code_point = 0x10000},
}};
constexpr std::uint8_t kContinuationMask = 0xC0;
constexpr std::uint8_t kContinuationTag = 0x80;
constexpr std::uint8_t kContinuationPayload = 0x3F;
constexpr unsigned kContinuationBits = 6;
constexpr std::uint32_t kMaxCodePoint = 0x10FFFF;
constexpr std::uint32_t kFirstSurrogate = 0xD800;
constexpr std::uint32_t kLastSurrogate = 0xDFFF;

// Decodes one multi-byte sequence at the start of `text` and returns its
// length, or 0 if it is not valid UTF-8.
std::size_t utf8_sequence_length(std::string_view text) {
  const auto lead = static_cast<std::uint8_t>(text.front());
  for (const Utf8Lead& form : kUtf8Leads) {
    if ((lead & form.mask) != form.tag) {
      continue;
    }
    if (text.size() < form.length) {
      return 0;
    }
    std::uint32_t code_point = lead & static_cast<std::uint8_t>(~form.mask);
    for (const char c : text.substr(1, form.length - 1)) {
      const auto continuation = static_cast<std::uint8_t>(c);
      if ((continuation & kContinuationMask) != kContinuationTag) {
        return 0;
      }
      code_point = (code_point << kContinuationBits) | (continuation & kContinuationPayload);
    }
    const bool is_surrogate = code_point >= kFirstSurrogate && code_point <= kLastSurrogate;
    if (code_point < form.min_code_point || code_point > kMaxCodePoint || is_surrogate) {
      return 0;
    }
    return form.length;
  }
  return 0;
}

// Strict UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF and
// no truncated sequences.
bool is_valid_utf8(std::string_view text) {
  while (!text.empty()) {
    if (static_cast<std::uint8_t>(text.front()) < kAsciiEnd) {
      text.remove_prefix(1);
      continue;
    }
    const std::size_t length = utf8_sequence_length(text);
    if (length == 0) {
      return false;
    }
    text.remove_prefix(length);
  }
  return true;
}

constexpr std::uint8_t kFirstPrintable = 0x20;
constexpr std::uint8_t kDelete = 0x7F;

bool is_forbidden_char(char c) {
  const auto byte = static_cast<std::uint8_t>(c);
  if (byte < kFirstPrintable || byte == kDelete) {
    return true;
  }
  constexpr std::string_view kWindowsForbidden = "<>:\"|?*";
  return kWindowsForbidden.find(c) != std::string_view::npos;
}

char ascii_upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

bool equals_ignoring_ascii_case(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (ascii_upper(a[i]) != ascii_upper(b[i])) {
      return false;
    }
  }
  return true;
}

// Windows opens a device instead of a file for these names, with or without
// an extension ("nul", "NUL.png").
bool is_windows_device_name(std::string_view segment) {
  std::string_view stem = segment.substr(0, segment.find('.'));
  while (stem.ends_with(' ')) {
    stem.remove_suffix(1);
  }
  constexpr std::array<std::string_view, 6> kNames = {"CON", "PRN",    "AUX",
                                                      "NUL", "CONIN$", "CONOUT$"};
  for (const std::string_view name : kNames) {
    if (equals_ignoring_ascii_case(stem, name)) {
      return true;
    }
  }
  if (stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9') {
    const std::string_view prefix = stem.substr(0, 3);
    return equals_ignoring_ascii_case(prefix, "COM") || equals_ignoring_ascii_case(prefix, "LPT");
  }
  return false;
}

// A segment other than "." and "..": Windows strips a trailing dot or space,
// which would make two different virtual paths name the same file.
bool is_valid_segment(std::string_view segment) {
  return !segment.ends_with('.') && !segment.ends_with(' ') && !is_windows_device_name(segment);
}

}  // namespace

std::optional<std::string> normalize_virtual_path(std::string_view path) {
  // A leading separator is a POSIX root or a UNC prefix; a colon (rejected
  // with the other forbidden characters) is a drive letter or an NTFS stream.
  // Either way the path would leave the mount.
  if (path.starts_with('/') || path.starts_with('\\')) {
    return std::nullopt;
  }
  for (const char c : path) {
    if (is_forbidden_char(c)) {
      return std::nullopt;
    }
  }
  if (!is_valid_utf8(path)) {
    return std::nullopt;
  }

  std::vector<std::string_view> segments;
  std::size_t begin = 0;
  while (begin <= path.size()) {
    std::size_t end = path.find_first_of("/\\", begin);
    if (end == std::string_view::npos) {
      end = path.size();
    }
    const std::string_view segment = path.substr(begin, end - begin);
    begin = end + 1;
    if (segment.empty() || segment == ".") {
      continue;
    }
    if (segment == "..") {
      if (segments.empty()) {
        return std::nullopt;
      }
      segments.pop_back();
      continue;
    }
    if (!is_valid_segment(segment)) {
      return std::nullopt;
    }
    segments.push_back(segment);
  }

  std::string result;
  for (const std::string_view segment : segments) {
    if (!result.empty()) {
      result += '/';
    }
    result += segment;
  }
  return result;
}

}  // namespace vnmaki
