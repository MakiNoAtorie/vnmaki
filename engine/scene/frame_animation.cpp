#include "engine/scene/frame_animation.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace vnmaki {

namespace {

// Lets a time that a sum of steps left a hair short of a frame boundary count as on it.
constexpr double kBoundaryTolerance = 1e-9;

}  // namespace

std::size_t AnimationDefinition::frame_at(double elapsed) const {
  const auto count = static_cast<double>(frames.size());
  const double passed = std::floor(std::max(elapsed, 0.0) * fps + kBoundaryTolerance);
  return static_cast<std::size_t>(loop ? std::fmod(passed, count) : std::min(passed, count - 1));
}

RectF uv_within(const RectF& frame, const RectF& part) {
  return RectF{.x = frame.x + part.x * frame.width,
               .y = frame.y + part.y * frame.height,
               .width = part.width * frame.width,
               .height = part.height * frame.height};
}

void FrameAnimation::finish() {
  if (!definition_->loop) {
    elapsed_ = std::max(elapsed_, definition_->duration());
  }
}

std::vector<std::string> FrameAnimation::missing_paths() const {
  std::vector<std::string> missing;
  for (const AnimationDefinition::Frame& frame : definition_->frames) {
    if (!textures_.contains(frame.path) &&
        std::ranges::find(missing, frame.path) == missing.end()) {
      missing.push_back(frame.path);
    }
  }
  return missing;
}

void FrameAnimation::set_texture(const std::string& path, TextureId texture, Size size) {
  textures_[path] = Texture{.id = texture, .size = size};
}

const FrameAnimation::Texture* FrameAnimation::current_texture() const {
  const auto found = textures_.find(definition_->frames.at(frame()).path);
  return found == textures_.end() ? nullptr : &found->second;
}

void FrameAnimation::draw(IRenderer& renderer) const {
  const Texture* texture = current_texture();
  if (texture == nullptr) {
    return;
  }
  Color tint = properties().tint;
  tint.a *= properties().alpha;
  renderer.draw(Quad{.corners = place_corners(properties(), definition_->frame_size(texture->size)),
                     .uv = uv_within(definition_->frames.at(frame()).uv, properties().crop),
                     .tint = tint},
                draw_state(texture->id));
}

RectF FrameAnimation::bounds() const {
  const Texture* texture = current_texture();
  const Size size = texture != nullptr ? definition_->frame_size(texture->size) : Size{};
  return bounding_box(place_corners(properties(), size));
}

}  // namespace vnmaki
