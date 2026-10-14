#include "engine/text/markup.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vnmaki {

namespace {

/// A decimal number without sign or exponent ("12", "0.5"). Read by hand, so no locale applies.
std::optional<double> parse_number(std::string_view text) {
  constexpr double kBase = 10;
  // A double holds this many decimal digits exactly, so the result is the nearest double.
  constexpr std::size_t kMaxLength = 15;
  if (text.size() > kMaxLength) {
    return std::nullopt;
  }
  double digits = 0;
  double scale = 1;
  bool in_fraction = false;
  bool has_digit = false;
  for (const char c : text) {
    if (c == '.' && !in_fraction) {
      in_fraction = true;
      continue;
    }
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    has_digit = true;
    digits = (digits * kBase) + static_cast<double>(c - '0');
    if (in_fraction) {
      scale *= kBase;
    }
  }
  if (!has_digit) {
    return std::nullopt;
  }
  return digits / scale;
}

std::optional<int> hex_digit(char c) {
  constexpr int kTen = 10;
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + kTen;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + kTen;
  }
  return std::nullopt;
}

/// "#rgb", "#rgba", "#rrggbb" or "#rrggbbaa".
std::optional<Color> parse_color(std::string_view text) {
  constexpr std::size_t kShort = 3;
  constexpr std::size_t kShortAlpha = 4;
  constexpr std::size_t kLong = 6;
  constexpr std::size_t kLongAlpha = 8;
  constexpr int kSixteen = 16;
  // One hex digit stands for the same digit twice: "f" is "ff".
  constexpr int kRepeatDigit = 17;
  if (text.empty() || text.front() != '#') {
    return std::nullopt;
  }
  text.remove_prefix(1);
  const bool is_short = text.size() == kShort || text.size() == kShortAlpha;
  if (!is_short && text.size() != kLong && text.size() != kLongAlpha) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> channels;
  channels.reserve(kShortAlpha);
  for (std::size_t i = 0; i < text.size(); i += is_short ? 1 : 2) {
    const std::optional<int> high = hex_digit(text[i]);
    const std::optional<int> low = is_short ? high : hex_digit(text[i + 1]);
    if (!high || !low) {
      return std::nullopt;
    }
    channels.push_back(
        static_cast<std::uint8_t>(is_short ? *high * kRepeatDigit : (*high * kSixteen) + *low));
  }
  if (channels.size() == kShort) {
    channels.push_back(UINT8_MAX);
  }
  return rgba8(channels[0], channels[1], channels[2], channels[3]);
}

std::string quoted(std::string_view tag) { return "{" + std::string{tag} + "}"; }

/// The number a tag needs: over 0 (or from 0, for `allow_zero`) and at most `max`.
double tag_number(std::string_view tag, std::string_view value, std::size_t offset, double max,
                  bool allow_zero, std::string_view unit) {
  const std::optional<double> number = parse_number(value);
  if (!number || *number > max || (*number <= 0 && !(allow_zero && *number == 0))) {
    throw MarkupError{offset, quoted(tag) + ": " + std::string{unit} + " must be a number " +
                                  (allow_zero ? "from 0" : "over 0") + " and at most " +
                                  std::to_string(static_cast<long long>(max)) +
                                  ", written as digits (for example 0.5)"};
  }
  return *number;
}

struct OpenTag {
  std::string_view name;
  std::size_t offset = 0;
  /// The style to go back to when the tag closes.
  TextStyle outer;
};

}  // namespace

StyledText parse_markup(std::string_view markup) {
  StyledText styled;
  TextStyle style;
  std::vector<OpenTag> open;
  std::size_t run_begin = 0;
  // Ends the run of `style` at the end of the text so far.
  const auto end_run = [&] {
    const std::size_t end = styled.text.size();
    if (end == run_begin) {
      return;
    }
    // Each ruby tag stays its own run: its furigana goes above its own base text.
    if (!styled.runs.empty() && style.ruby.empty() && styled.runs.back().style == style) {
      styled.runs.back().end = end;
    } else {
      styled.runs.push_back(StyledRun{.begin = run_begin, .end = end, .style = style});
    }
    run_begin = end;
  };

  std::size_t position = 0;
  while (position < markup.size()) {
    const std::size_t brace = markup.find('{', position);
    styled.text.append(markup.substr(position, brace - position));
    if (brace == std::string_view::npos) {
      break;
    }
    if (markup.substr(brace, 2) == "{{") {
      styled.text.push_back('{');
      position = brace + 2;
      continue;
    }
    const std::size_t close = markup.find_first_of("{}", brace + 1);
    if (close == std::string_view::npos || markup[close] == '{') {
      throw MarkupError{brace, "a tag must end with '}' (write {{ for a plain brace)"};
    }
    const std::string_view tag = markup.substr(brace + 1, close - brace - 1);
    position = close + 1;
    end_run();

    if (tag.starts_with('/')) {
      if (open.empty()) {
        throw MarkupError{brace, quoted(tag) + " closes no tag"};
      }
      if (open.back().name != tag.substr(1)) {
        throw MarkupError{brace, quoted(tag) + " does not match " + quoted(open.back().name)};
      }
      style = std::move(open.back().outer);
      open.pop_back();
      continue;
    }

    const std::size_t equals = tag.find('=');
    const bool has_value = equals != std::string_view::npos;
    const std::string_view name = tag.substr(0, equals);
    const std::string_view value = has_value ? tag.substr(equals + 1) : std::string_view{};
    if (name == "pause") {
      styled.pauses.push_back(
          TextPause{.offset = styled.text.size(),
                    .seconds = tag_number(tag, value, brace, kMaxTagNumber, true, "the pause")});
      continue;
    }
    TextStyle outer = style;
    if (name == "b" || name == "i") {
      if (has_value) {
        throw MarkupError{brace, quoted(tag) + ": {" + std::string{name} + "} takes no value"};
      }
      (name == "b" ? style.bold : style.italic) = true;
    } else if (name == "color") {
      const std::optional<Color> color = parse_color(value);
      if (!color) {
        throw MarkupError{brace,
                          quoted(tag) + ": the color must be #rgb, #rgba, #rrggbb or #rrggbbaa"};
      }
      style.color = color;
    } else if (name == "size") {
      style.size = static_cast<float>(
          tag_number(tag, value, brace, double{kMaxFontPixelSize}, false, "the size"));
    } else if (name == "speed") {
      style.speed = tag_number(tag, value, brace, kMaxTagNumber, false, "the speed");
    } else if (name == "font" || name == "ruby" || name == "link") {
      if (value.empty()) {
        throw MarkupError{brace, quoted(tag) + ": {" + std::string{name} + "=...} needs a value"};
      }
      std::string& field = name == "font" ? style.font : (name == "ruby" ? style.ruby : style.link);
      if (name != "font" && !field.empty()) {
        throw MarkupError{brace, quoted(tag) + ": " + std::string{name} + " tags cannot be nested"};
      }
      field = value;
    } else {
      throw MarkupError{brace, "unknown tag '" + std::string{name} + "'"};
    }
    open.push_back(OpenTag{.name = name, .offset = brace, .outer = std::move(outer)});
  }
  end_run();
  if (!open.empty()) {
    throw MarkupError{open.back().offset, quoted(open.back().name) + " is not closed"};
  }
  return styled;
}

std::string escape_markup(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (const char c : text) {
    escaped.push_back(c);
    if (c == '{') {
      escaped.push_back(c);
    }
  }
  return escaped;
}

StyledText plain_text(std::string_view text, TextStyle style) {
  StyledText styled;
  styled.text = text;
  if (!text.empty()) {
    styled.runs.push_back(StyledRun{.begin = 0, .end = text.size(), .style = std::move(style)});
  }
  return styled;
}

}  // namespace vnmaki
