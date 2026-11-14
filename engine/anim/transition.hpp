#pragma once

// Transitions (docs/SPEC.md section 10): a change of the scene that blends the old frame into the
// new one over a few seconds. The scene draws both frames into render targets and one shader mixes
// them (engine/scene/scene.hpp). This file has what does not need a GPU: the built-in names, the
// options, the clock and the constant buffer, and the built-in shaders.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/runtime/script_runtime.hpp"

namespace vnmaki {

/// How long a transition takes when the script gives no `duration`.
inline constexpr double kDefaultTransitionSeconds = 0.5;

enum class TransitionKind : std::uint8_t {
  kDissolve,  ///< "dissolve": the old frame fades into the new one.
  kFade,      ///< "fade": through a colour (option `color`, black by default).
  kSlide,     ///< "slide_left", "slide_right", "slide_up", "slide_down".
  kWipe,      ///< "wipe_left", "wipe_right", "wipe_up", "wipe_down": options `softness`.
  kMask,      ///< "mask": options `mask` (an image name) and `softness`.
  kCustom,    ///< Any other name: a game shader that declares the uniform "progress".
};

/// The built-in names, for error messages.
inline constexpr std::string_view kBuiltinTransitions =
    "dissolve, fade, slide_left, slide_right, slide_up, slide_down, wipe_left, wipe_right, "
    "wipe_up, wipe_down and mask";

/// A transition that is checked. The direction is the way the content (slide) or the edge (wipe)
/// moves, in texture coordinates: left is (-1, 0), up is (0, -1).
struct TransitionSpec {
  TransitionKind kind = TransitionKind::kDissolve;
  std::string name{};
  PointF direction{};
  Color color{.r = 0, .g = 0, .b = 0, .a = 1};
  /// The image name that the script gave; the scene finds the file.
  std::string mask{};
  float softness = 0;
  double duration = kDefaultTransitionSeconds;
};

/// Checks the name and the options of `request`, without a GPU or a VFS: a name that is not
/// built-in gives kCustom, which the scene checks against its shaders. Returns why a request is
/// bad (an option that the transition does not take, a number out of range, a `mask` without
/// an image), or an empty string.
[[nodiscard]] std::string make_transition_spec(const TransitionRequest& request,
                                               TransitionSpec& spec);

/// The constant buffer of the built-in transition shaders (shaders/transition_uniforms.hlsli).
struct TransitionUniforms {
  float progress = 0;
  float softness = 0;
  float direction_x = 0;
  float direction_y = 0;
  /// Straight alpha.
  std::array<float, 4> color{};
};
static_assert(sizeof(TransitionUniforms) == 32);

/// The clock of a running transition. It has no GPU and no real clock: the caller gives the
/// elapsed seconds to advance(), so tests use a fake clock.
class Transition {
 public:
  explicit Transition(TransitionSpec spec) : spec_(std::move(spec)) {}

  void advance(double seconds) { elapsed_ += seconds; }
  /// 0 at the start and 1 at the end, linear in time.
  [[nodiscard]] double progress() const;
  /// True once the duration has passed (always for a duration of 0).
  [[nodiscard]] bool finished() const;
  [[nodiscard]] const TransitionSpec& spec() const { return spec_; }
  /// The constant buffer of a built-in transition at the current progress.
  [[nodiscard]] TransitionUniforms uniforms() const;

 private:
  TransitionSpec spec_;
  double elapsed_ = 0;
};

/// The GPU shaders of the built-in transitions, made when first asked for.
class TransitionShaders {
 public:
  /// The shader of a built-in kind (not kCustom). Throws RenderError.
  [[nodiscard]] ShaderId get(IRenderer& renderer, TransitionKind kind);
  /// The shader of a built-in kind if get() made it.
  [[nodiscard]] std::optional<ShaderId> find(TransitionKind kind) const;
  /// Destroys the shaders that were made.
  void release(IRenderer& renderer);

 private:
  std::array<std::optional<ShaderId>, 5> shaders_{};
};

}  // namespace vnmaki
