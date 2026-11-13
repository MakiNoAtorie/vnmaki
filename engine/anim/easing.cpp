#include "engine/anim/easing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>
#include <string_view>

namespace vnmaki {

namespace {

constexpr std::array<std::string_view, 16> kNames{
    "linear",      "in_sine",   "out_sine",   "in_out_sine",   "in_quad", "out_quad",
    "in_out_quad", "in_cubic",  "out_cubic",  "in_out_cubic",  "in_back", "out_back",
    "in_out_back", "in_bounce", "out_bounce", "in_out_bounce",
};

constexpr double kHalf = 0.5;
// The overshoot of the back curves, and the scale of their in-out form (Robert Penner's).
constexpr double kBackOvershoot = 1.70158;
constexpr double kBackInOutScale = 1.525;
// The bounce curve is four parabolas. Each one has this scale. Divided by the divisor, the
// numbers are where each bounce ends and where its parabola is centred; the floor is the
// height it returns to.
constexpr double kBounceScale = 7.5625;
constexpr double kBounceDivisor = 2.75;
constexpr double kBounceEnd1 = 1.0;
constexpr double kBounceEnd2 = 2.0;
constexpr double kBounceEnd3 = 2.5;
constexpr double kBounceCentre2 = 1.5;
constexpr double kBounceCentre3 = 2.25;
constexpr double kBounceCentre4 = 2.625;
constexpr double kBounceFloor2 = 0.75;
constexpr double kBounceFloor3 = 0.9375;
constexpr double kBounceFloor4 = 0.984375;

double out_bounce(double t) {
  if (t < kBounceEnd1 / kBounceDivisor) {
    return kBounceScale * t * t;
  }
  if (t < kBounceEnd2 / kBounceDivisor) {
    const double u = t - kBounceCentre2 / kBounceDivisor;
    return kBounceScale * u * u + kBounceFloor2;
  }
  if (t < kBounceEnd3 / kBounceDivisor) {
    const double u = t - kBounceCentre3 / kBounceDivisor;
    return kBounceScale * u * u + kBounceFloor3;
  }
  const double u = t - kBounceCentre4 / kBounceDivisor;
  return kBounceScale * u * u + kBounceFloor4;
}

double in_back(double t) { return (kBackOvershoot + 1) * t * t * t - kBackOvershoot * t * t; }

double out_back(double t) {
  const double u = t - 1;
  return 1 + (kBackOvershoot + 1) * u * u * u + kBackOvershoot * u * u;
}

double in_out_back(double t) {
  const double scale = kBackOvershoot * kBackInOutScale;
  if (t < kHalf) {
    const double u = 2 * t;
    return (u * u * ((scale + 1) * u - scale)) / 2;
  }
  const double u = 2 * t - 2;
  return (u * u * ((scale + 1) * u + scale) + 2) / 2;
}

}  // namespace

std::string_view ease_name(Ease ease) { return kNames.at(static_cast<std::size_t>(ease)); }

std::optional<Ease> ease_from_name(std::string_view name) {
  const auto found = std::find(kNames.begin(), kNames.end(), name);
  if (found == kNames.end()) {
    return std::nullopt;
  }
  return static_cast<Ease>(found - kNames.begin());
}

double apply_ease(Ease ease, double progress) {
  // NaN passes std::clamp unchanged: make it 0.
  const double t = std::isnan(progress) ? 0.0 : std::clamp(progress, 0.0, 1.0);
  switch (ease) {
    case Ease::kLinear:
      return t;
    case Ease::kInSine:
      return 1 - std::cos(t * std::numbers::pi * kHalf);
    case Ease::kOutSine:
      return std::sin(t * std::numbers::pi * kHalf);
    case Ease::kInOutSine:
      return -(std::cos(std::numbers::pi * t) - 1) / 2;
    case Ease::kInQuad:
      return t * t;
    case Ease::kOutQuad:
      return 1 - (1 - t) * (1 - t);
    case Ease::kInOutQuad:
      return t < kHalf ? 2 * t * t : 1 - ((2 - 2 * t) * (2 - 2 * t)) / 2;
    case Ease::kInCubic:
      return t * t * t;
    case Ease::kOutCubic:
      return 1 - std::pow(1 - t, 3);
    case Ease::kInOutCubic:
      return t < kHalf ? 4 * t * t * t : 1 - std::pow(2 - 2 * t, 3) / 2;
    case Ease::kInBack:
      return in_back(t);
    case Ease::kOutBack:
      return out_back(t);
    case Ease::kInOutBack:
      return in_out_back(t);
    case Ease::kInBounce:
      return 1 - out_bounce(1 - t);
    case Ease::kOutBounce:
      return out_bounce(t);
    case Ease::kInOutBounce:
      return t < kHalf ? (1 - out_bounce(1 - 2 * t)) / 2 : (1 + out_bounce(2 * t - 1)) / 2;
  }
  return t;
}

}  // namespace vnmaki
