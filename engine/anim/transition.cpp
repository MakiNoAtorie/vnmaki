#include "engine/anim/transition.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "engine/anim/timeline.hpp"
#include "engine/render/builtin_shaders.hpp"

namespace vnmaki {

namespace {

constexpr double kClockSlack = 1e-9;  // a sum of fake-clock steps can fall just short of the end
constexpr unsigned kRedShift = 16;
constexpr unsigned kGreenShift = 8;
constexpr std::uint32_t kByteMask = 0xFF;

struct Named {
  std::string_view name{};
  TransitionKind kind = TransitionKind::kDissolve;
  PointF direction{};
};

constexpr PointF kLeft{.x = -1, .y = 0};
constexpr PointF kRight{.x = 1, .y = 0};
constexpr PointF kUp{.x = 0, .y = -1};
constexpr PointF kDown{.x = 0, .y = 1};

constexpr std::array kBuiltins{
    Named{.name = "dissolve", .kind = TransitionKind::kDissolve},
    Named{.name = "fade", .kind = TransitionKind::kFade},
    Named{.name = "mask", .kind = TransitionKind::kMask},
    Named{.name = "slide_left", .kind = TransitionKind::kSlide, .direction = kLeft},
    Named{.name = "slide_right", .kind = TransitionKind::kSlide, .direction = kRight},
    Named{.name = "slide_up", .kind = TransitionKind::kSlide, .direction = kUp},
    Named{.name = "slide_down", .kind = TransitionKind::kSlide, .direction = kDown},
    Named{.name = "wipe_left", .kind = TransitionKind::kWipe, .direction = kLeft},
    Named{.name = "wipe_right", .kind = TransitionKind::kWipe, .direction = kRight},
    Named{.name = "wipe_up", .kind = TransitionKind::kWipe, .direction = kUp},
    Named{.name = "wipe_down", .kind = TransitionKind::kWipe, .direction = kDown},
};

std::string not_for(std::string_view option, std::string_view name) {
  std::string message = "option '";
  message += option;
  message += "' does not apply to the transition '";
  message += name;
  message += "'";
  return message;
}

}  // namespace

std::string make_transition_spec(const TransitionRequest& request, TransitionSpec& spec) {
  const auto found = std::ranges::find(kBuiltins, request.name, &Named::name);
  spec = TransitionSpec{.name = request.name};
  if (found != kBuiltins.end()) {
    spec.kind = found->kind;
    spec.direction = found->direction;
  } else {
    spec.kind = TransitionKind::kCustom;
  }
  if (request.duration.has_value()) {
    if (!(*request.duration >= 0 && *request.duration <= kMaxTweenSeconds)) {
      return "option 'duration' must be a number from 0 to 86400";
    }
    spec.duration = *request.duration;
  }
  const bool soft = spec.kind == TransitionKind::kWipe || spec.kind == TransitionKind::kMask;
  if (request.softness.has_value()) {
    if (!soft) {
      return not_for("softness", request.name);
    }
    if (!(*request.softness >= 0 && *request.softness <= 1)) {
      return "option 'softness' must be a number from 0 to 1";
    }
    spec.softness = static_cast<float>(*request.softness);
  }
  if (request.color.has_value()) {
    if (spec.kind != TransitionKind::kFade) {
      return not_for("color", request.name);
    }
    spec.color = rgba8(static_cast<std::uint8_t>((*request.color >> kRedShift) & kByteMask),
                       static_cast<std::uint8_t>((*request.color >> kGreenShift) & kByteMask),
                       static_cast<std::uint8_t>(*request.color & kByteMask));
  }
  if (spec.kind == TransitionKind::kMask) {
    if (!request.mask.has_value() || request.mask->empty()) {
      return "the transition 'mask' needs the option 'mask' (an image name)";
    }
    spec.mask = *request.mask;
  } else if (request.mask.has_value()) {
    return not_for("mask", request.name);
  }
  if (!request.uniforms.empty() && spec.kind != TransitionKind::kCustom) {
    return not_for("uniforms", request.name);
  }
  return {};
}

double Transition::progress() const {
  return spec_.duration > 0 ? std::clamp(elapsed_ / spec_.duration, 0.0, 1.0) : 1.0;
}

bool Transition::finished() const { return elapsed_ + kClockSlack >= spec_.duration; }

TransitionUniforms Transition::uniforms() const {
  return TransitionUniforms{.progress = static_cast<float>(progress()),
                            .softness = spec_.softness,
                            .direction_x = spec_.direction.x,
                            .direction_y = spec_.direction.y,
                            .color = {spec_.color.r, spec_.color.g, spec_.color.b, spec_.color.a}};
}

ShaderId TransitionShaders::get(IRenderer& renderer, TransitionKind kind) {
  if (kind == TransitionKind::kCustom) {
    throw RenderError{"a custom transition has no built-in shader"};
  }
  std::optional<ShaderId>& made = shaders_.at(static_cast<std::size_t>(kind));
  if (!made.has_value()) {
    switch (kind) {
      case TransitionKind::kDissolve:
        made = renderer.create_shader(shaders::transition_dissolve_frag());
        break;
      case TransitionKind::kFade:
        made = renderer.create_shader(shaders::transition_fade_frag());
        break;
      case TransitionKind::kSlide:
        made = renderer.create_shader(shaders::transition_slide_frag());
        break;
      case TransitionKind::kWipe:
        made = renderer.create_shader(shaders::transition_wipe_frag());
        break;
      case TransitionKind::kMask:
        made = renderer.create_shader(shaders::transition_mask_frag());
        break;
      case TransitionKind::kCustom:
        break;
    }
  }
  return *made;
}

std::optional<ShaderId> TransitionShaders::find(TransitionKind kind) const {
  return kind == TransitionKind::kCustom ? std::nullopt
                                         : shaders_.at(static_cast<std::size_t>(kind));
}

void TransitionShaders::release(IRenderer& renderer) {
  for (std::optional<ShaderId>& made : shaders_) {
    if (made.has_value()) {
      renderer.destroy_shader(*made);
      made.reset();
    }
  }
}

}  // namespace vnmaki
