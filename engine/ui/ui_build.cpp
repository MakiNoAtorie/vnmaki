#include "engine/ui/ui_build.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace vnmaki {

namespace {

constexpr double kMaxTextSize = 1024;  // the limit of layout_text()
constexpr std::size_t kRgbDigits = 6;
constexpr std::size_t kRgbaDigits = 8;
constexpr std::string_view kHexDigits = "0123456789abcdef";
constexpr std::uint32_t kHexBits = 4;
constexpr int kByteBits = 8;
constexpr std::uint32_t kByteMask = 0xFF;

// The look of a button until themes (a later ticket) set it.
constexpr float kButtonPadding = 12;
const Color kButtonColor = rgba8(40, 44, 72);
const Color kButtonFocusColor = rgba8(96, 112, 200);

// The look of the choice menu, as fractions of the canvas.
constexpr int kMenuWidthDivisor = 2;  // button width: width / 2
constexpr int kMenuTextDivisor = 24;  // text size: height / 24
constexpr int kMenuGapDivisor = 40;   // space between buttons: height / 40
constexpr int kMenuPadDivisor = 48;   // button padding: height / 48
const Color kMenuDimColor = rgba8(0, 0, 0, 128);

std::string describe(const ScreenNodeDef& def) {
  return def.text.empty() ? def.kind : def.kind + " \"" + def.text + "\"";
}

[[noreturn]] void bad_option(const ScreenNodeDef& def, const std::string& name,
                             std::string_view expected) {
  throw UiBuildError(describe(def) + ": option '" + name + "' needs " + std::string{expected});
}

std::optional<NodeKind> kind_from_name(std::string_view name) {
  if (name == "box") {
    return NodeKind::kBox;
  }
  if (name == "hbox") {
    return NodeKind::kHbox;
  }
  if (name == "vbox") {
    return NodeKind::kVbox;
  }
  if (name == "text") {
    return NodeKind::kText;
  }
  if (name == "image") {
    return NodeKind::kImage;
  }
  if (name == "button") {
    return NodeKind::kButton;
  }
  return std::nullopt;
}

std::optional<double> as_number(const ScreenValue& value) {
  if (const auto* integer = std::get_if<std::int64_t>(&value)) {
    return static_cast<double>(*integer);
  }
  if (const auto* number = std::get_if<double>(&value)) {
    return *number;
  }
  return std::nullopt;
}

// A length in virtual pixels: a finite number that is not negative.
float read_length(const ScreenNodeDef& def, const std::string& name, const ScreenValue& value) {
  const std::optional<double> number = as_number(value);
  if (!number || !std::isfinite(*number) || *number < 0) {
    bad_option(def, name, "a number that is not negative");
  }
  return static_cast<float>(*number);
}

float read_offset(const ScreenNodeDef& def, const std::string& name, const ScreenValue& value) {
  const std::optional<double> number = as_number(value);
  if (!number || !std::isfinite(*number)) {
    bad_option(def, name, "a number");
  }
  return static_cast<float>(*number);
}

std::string read_string(const ScreenNodeDef& def, const std::string& name,
                        const ScreenValue& value, std::string_view expected) {
  const auto* text = std::get_if<std::string>(&value);
  if (text == nullptr) {
    bad_option(def, name, expected);
  }
  return *text;
}

Color read_color(const ScreenNodeDef& def, const std::string& name, const ScreenValue& value) {
  constexpr std::string_view kExpected = "a colour \"#rrggbb\" or \"#rrggbbaa\"";
  std::string text = read_string(def, name, value, kExpected);
  std::ranges::transform(text, text.begin(), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  if ((text.size() != kRgbDigits + 1 && text.size() != kRgbaDigits + 1) || text.front() != '#') {
    bad_option(def, name, kExpected);
  }
  std::uint32_t channels = 0;
  for (const char digit : std::string_view{text}.substr(1)) {
    const std::size_t digit_value = kHexDigits.find(digit);
    if (digit_value == std::string_view::npos) {
      bad_option(def, name, kExpected);
    }
    channels = (channels << kHexBits) | static_cast<std::uint32_t>(digit_value);
  }
  if (text.size() == kRgbDigits + 1) {
    channels = (channels << kByteBits) | kByteMask;
  }
  const auto channel = [&](int shift) {
    return static_cast<std::uint8_t>((channels >> shift) & kByteMask);
  };
  return rgba8(channel(3 * kByteBits), channel(2 * kByteBits), channel(kByteBits), channel(0));
}

Align read_align(const ScreenNodeDef& def, const std::string& name, const ScreenValue& value) {
  const std::string text =
      read_string(def, name, value, "\"stretch\", \"start\", \"center\" or \"end\"");
  if (text == "stretch") {
    return Align::kStretch;
  }
  if (text == "start") {
    return Align::kStart;
  }
  if (text == "center") {
    return Align::kCenter;
  }
  if (text == "end") {
    return Align::kEnd;
  }
  bad_option(def, name, "\"stretch\", \"start\", \"center\" or \"end\"");
}

Justify read_justify(const ScreenNodeDef& def, const std::string& name, const ScreenValue& value) {
  constexpr std::string_view kExpected = "\"start\", \"center\", \"end\" or \"space_between\"";
  const std::string text = read_string(def, name, value, kExpected);
  if (text == "start") {
    return Justify::kStart;
  }
  if (text == "center") {
    return Justify::kCenter;
  }
  if (text == "end") {
    return Justify::kEnd;
  }
  if (text == "space_between") {
    return Justify::kSpaceBetween;
  }
  bad_option(def, name, kExpected);
}

void apply_option(const ScreenNodeDef& def, UiNode& node, const std::string& name,
                  const ScreenValue& value, PointF& position, bool& has_position) {
  UiStyle& style = node.style;
  const bool is_button = node.kind == NodeKind::kButton;
  if (name == "width") {
    style.width = read_length(def, name, value);
  } else if (name == "height") {
    style.height = read_length(def, name, value);
  } else if (name == "grow") {
    style.grow = read_length(def, name, value);
  } else if (name == "padding") {
    style.padding = read_length(def, name, value);
  } else if (name == "margin") {
    style.margin = read_length(def, name, value);
  } else if (name == "gap") {
    style.gap = read_length(def, name, value);
  } else if (name == "align") {
    style.align = read_align(def, name, value);
  } else if (name == "justify") {
    style.justify = read_justify(def, name, value);
  } else if (name == "x") {
    position.x = read_offset(def, name, value);
    has_position = true;
  } else if (name == "y") {
    position.y = read_offset(def, name, value);
    has_position = true;
  } else if (name == "bg") {
    style.background = read_color(def, name, value);
  } else if (name == "color") {
    style.color = read_color(def, name, value);
  } else if (name == "size") {
    const std::optional<double> size = as_number(value);
    if (!size || !(*size > 0) || *size > kMaxTextSize) {
      bad_option(def, name, "a number above 0 and at most 1024");
    }
    style.text_size = static_cast<float>(*size);
  } else if (name == "label") {
    node.label = read_string(def, name, value, "a string");
  } else if (is_button && name == "returns") {
    node.returns = value;
  } else if (is_button && name == "focus_bg") {
    style.focus_background = read_color(def, name, value);
  } else if (is_button && name == "disabled") {
    const auto* flag = std::get_if<bool>(&value);
    if (flag == nullptr) {
      bad_option(def, name, "a boolean");
    }
    node.state.disabled = *flag;
  } else {
    throw UiBuildError(describe(def) + ": unknown option '" + name + "'");
  }
}

UiNode build(const ScreenNodeDef& def) {
  const std::optional<NodeKind> kind = kind_from_name(def.kind);
  if (!kind) {
    throw UiBuildError("unknown node kind '" + def.kind + "'");
  }
  UiNode node = make_node(*kind);
  node.text = def.text;
  node.label = def.text;
  if (*kind == NodeKind::kButton) {
    node.style.background = kButtonColor;
    node.style.focus_background = kButtonFocusColor;
    node.style.padding = kButtonPadding;
  }
  PointF position;
  bool has_position = false;
  for (const auto& [name, value] : def.props) {
    apply_option(def, node, name, value, position, has_position);
  }
  if (has_position) {
    node.style.position = position;
  }
  if (*kind == NodeKind::kImage && (!node.style.width || !node.style.height)) {
    throw UiBuildError(describe(def) + ": an image needs the options width and height");
  }
  node.children.reserve(def.children.size());
  for (const ScreenNodeDef& child : def.children) {
    node.children.push_back(build(child));
  }
  return node;
}

}  // namespace

UiNode build_ui_tree(const ScreenNodeDef& def) { return build(def); }

UiNode make_menu_screen(const std::vector<std::string>& options, Size canvas) {
  UiNode root = make_node(NodeKind::kVbox);
  root.style.align = Align::kCenter;
  root.style.justify = Justify::kCenter;
  root.style.gap = static_cast<float>(canvas.height / kMenuGapDivisor);
  root.style.background = kMenuDimColor;
  root.children.reserve(options.size());
  for (std::size_t i = 0; i < options.size(); ++i) {
    UiNode button = make_node(NodeKind::kButton);
    button.text = options[i];
    button.label = options[i];
    button.returns = static_cast<std::int64_t>(i);
    button.style.width = static_cast<float>(canvas.width / kMenuWidthDivisor);
    button.style.padding = static_cast<float>(canvas.height / kMenuPadDivisor);
    button.style.text_size = static_cast<float>(canvas.height / kMenuTextDivisor);
    button.style.background = kButtonColor;
    button.style.focus_background = kButtonFocusColor;
    root.children.push_back(std::move(button));
  }
  return root;
}

}  // namespace vnmaki
