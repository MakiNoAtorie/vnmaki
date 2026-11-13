#pragma once

// Easing curves (docs/SPEC.md section 10): a function from linear progress to eased progress.

#include <cstdint>
#include <optional>
#include <string_view>

namespace vnmaki {

/// `kLinear`, then sine, quad, cubic, back and bounce, each as in, out and in-out.
enum class Ease : std::uint8_t {
  kLinear,
  kInSine,
  kOutSine,
  kInOutSine,
  kInQuad,
  kOutQuad,
  kInOutQuad,
  kInCubic,
  kOutCubic,
  kInOutCubic,
  kInBack,
  kOutBack,
  kInOutBack,
  kInBounce,
  kOutBounce,
  kInOutBounce,
};

/// The script names: "linear", "in_sine", "out_sine", "in_out_sine", "in_quad", ...
[[nodiscard]] std::string_view ease_name(Ease ease);
[[nodiscard]] std::optional<Ease> ease_from_name(std::string_view name);

/// The eased value of `progress`, which is clamped to [0, 1] first. Every curve gives 0 at 0 and
/// 1 at 1; back curves leave [0, 1] in between.
[[nodiscard]] double apply_ease(Ease ease, double progress);

}  // namespace vnmaki
