#include "plugins/example/example_plugin.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "engine/render/renderer.hpp"
#include "engine/scene/displayable.hpp"

namespace vnmaki {

namespace {

constexpr double kDefaultBadgeSize = 100;
constexpr double kMaxBadgeSize = 4096;
constexpr std::string_view kDefaultBadgeColor = "#ff8800";
constexpr std::size_t kColorLength = 7;  // "#rrggbb"
constexpr int kHexBase = 16;
constexpr float kMaxChannel = 255.0F;
constexpr std::size_t kChannels = 3;
constexpr int kPpmMaxValue = 255;
constexpr int kDecimalBase = 10;

// "#rrggbb" as a colour; throws PluginError.
Color parse_color(std::string_view text) {
  if (text.size() != kColorLength || text.front() != '#') {
    throw PluginError{"param 'color' must be \"#rrggbb\""};
  }
  std::array<float, kChannels> channels{};
  for (std::size_t i = 0; i < kChannels; ++i) {
    unsigned value = 0;
    const std::string_view pair = text.substr(1 + (i * 2), 2);
    const auto [stop, error] =
        std::from_chars(pair.data(), pair.data() + pair.size(), value, kHexBase);
    if (error != std::errc{} || stop != pair.data() + pair.size()) {
      throw PluginError{"param 'color' must be \"#rrggbb\""};
    }
    channels.at(i) = static_cast<float>(value) / kMaxChannel;
  }
  return Color{.r = channels[0], .g = channels[1], .b = channels[2], .a = 1};
}

// A square of one colour. It draws the scene's white texture, tinted.
class BadgeDisplayable final : public PluginDisplayable {
 public:
  BadgeDisplayable(float size, Color color) : size_(size), color_(color) {}

  void update(double /*seconds*/) override {}

  void draw(IRenderer& renderer) const override {
    if (white_texture() == TextureId{}) {
      return;
    }
    Color tint = color_;
    tint.a *= properties().alpha;
    renderer.draw(
        Quad{.corners = place_corners(properties(), box()), .uv = kWholeTexture, .tint = tint},
        draw_state(white_texture()));
  }

  [[nodiscard]] RectF bounds() const override {
    return bounding_box(place_corners(properties(), box()));
  }

 private:
  [[nodiscard]] Size box() const {
    return Size{.width = static_cast<int>(size_), .height = static_cast<int>(size_)};
  }

  float size_;
  Color color_;
};

std::unique_ptr<PluginDisplayable> make_badge(const PluginParams& params) {
  const double size = number_param(params, "size", kDefaultBadgeSize);
  if (!(size >= 1 && size <= kMaxBadgeSize)) {
    throw PluginError{"param 'size' must be from 1 to 4096"};
  }
  const Color color = parse_color(string_param(params, "color", kDefaultBadgeColor));
  return std::make_unique<BadgeDisplayable>(static_cast<float>(size), color);
}

PluginValue example_add(std::span<const PluginValue> args) {
  if (args.size() != 2) {
    throw PluginError{"needs two numbers"};
  }
  const auto* left_int = std::get_if<std::int64_t>(&args[0]);
  const auto* right_int = std::get_if<std::int64_t>(&args[1]);
  if (left_int != nullptr && right_int != nullptr) {
    return *left_int + *right_int;
  }
  const auto as_number = [](const PluginValue& value) -> double {
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
      return static_cast<double>(*integer);
    }
    if (const auto* real = std::get_if<double>(&value)) {
      return *real;
    }
    throw PluginError{"needs two numbers"};
  };
  return as_number(args[0]) + as_number(args[1]);
}

PluginValue example_greet(std::span<const PluginValue> args) {
  if (args.size() != 1 || !std::holds_alternative<std::string>(args[0])) {
    throw PluginError{"needs a name"};
  }
  return "Hello, " + std::get<std::string>(args[0]) + "!";
}

// Binary PPM: "P6", the width, the height and 255 (white space or # comments between them), one
// white space byte, then 3 bytes for each pixel.
class PpmReader {
 public:
  explicit PpmReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  PluginImage read() {
    if (bytes_.size() < 2 || next() != 'P' || next() != '6') {
      throw PluginError{"not a binary PPM (P6) file"};
    }
    PluginImage image;
    image.width = number();
    image.height = number();
    if (number() != kPpmMaxValue) {
      throw PluginError{"PPM: only a maximum value of 255 is supported"};
    }
    ++at_;  // the one white space byte after the header
    const auto pixels =
        static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
    if (bytes_.size() < at_ || bytes_.size() - at_ != pixels * kChannels) {
      throw PluginError{"PPM: the pixel data has the wrong size"};
    }
    image.rgba.reserve(pixels * 4);
    for (std::size_t i = 0; i < pixels; ++i) {
      for (std::size_t channel = 0; channel < kChannels; ++channel) {
        image.rgba.push_back(static_cast<std::uint8_t>(bytes_[at_++]));
      }
      image.rgba.push_back(std::uint8_t{kPpmMaxValue});
    }
    return image;
  }

 private:
  char next() { return at_ < bytes_.size() ? static_cast<char>(bytes_[at_++]) : '\0'; }

  int number() {
    while (at_ < bytes_.size()) {
      const char c = static_cast<char>(bytes_[at_]);
      if (c == '#') {
        while (at_ < bytes_.size() && static_cast<char>(bytes_[at_]) != '\n') {
          ++at_;
        }
      } else if (std::isspace(static_cast<unsigned char>(c)) != 0) {
        ++at_;
      } else {
        break;
      }
    }
    int value = 0;
    bool any = false;
    while (at_ < bytes_.size() && std::isdigit(static_cast<unsigned char>(bytes_[at_])) != 0) {
      value = (value * kDecimalBase) + (static_cast<char>(bytes_[at_]) - '0');
      any = true;
      ++at_;
      if (value > kMaxImageDimension) {
        throw PluginError{"PPM: the picture is too large"};
      }
    }
    if (!any) {
      throw PluginError{"PPM: bad header"};
    }
    return value;
  }

  std::span<const std::byte> bytes_;
  std::size_t at_ = 0;
};

}  // namespace

void register_plugin_example(PluginContext& context) {
  context.add_function("example_add", example_add);
  context.add_function("example_greet", example_greet);
  context.add_displayable("example_badge", make_badge);
  context.add_image_loader(
      ".ppm", [](std::span<const std::byte> bytes) { return PpmReader{bytes}.read(); });
}

}  // namespace vnmaki
