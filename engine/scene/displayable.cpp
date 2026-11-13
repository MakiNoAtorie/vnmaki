#include "engine/scene/displayable.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace vnmaki {

namespace {

constexpr float kDegreesPerHalfTurn = 180.0F;

}  // namespace

std::array<PointF, 4> place_corners(const DisplayProperties& properties, Size size, RectF part) {
  const auto width = static_cast<float>(size.width) * properties.crop.width;
  const auto height = static_cast<float>(size.height) * properties.crop.height;
  const float radians = properties.rotation * std::numbers::pi_v<float> / kDegreesPerHalfTurn;
  const float cosine = std::cos(radians);
  const float sine = std::sin(radians);
  const auto place = [&](float corner_x, float corner_y) {
    const float x = (corner_x - properties.anchor.x) * width * properties.scale.x;
    const float y = (corner_y - properties.anchor.y) * height * properties.scale.y;
    return PointF{.x = properties.position.x + x * cosine - y * sine,
                  .y = properties.position.y + x * sine + y * cosine};
  };
  const float right = part.x + part.width;
  const float bottom = part.y + part.height;
  return {place(part.x, part.y), place(right, part.y), place(right, bottom), place(part.x, bottom)};
}

void ImageDisplayable::set_texture(TextureId texture, Size size) {
  texture_ = texture;
  size_ = size;
}

void ImageDisplayable::draw(IRenderer& renderer) const {
  if (!has_texture()) {
    return;
  }
  Color tint = properties().tint;
  tint.a *= properties().alpha;
  renderer.draw(
      Quad{.corners = place_corners(properties(), size_), .uv = properties().crop, .tint = tint},
      draw_state(texture_));
}

RectF bounding_box(const std::array<PointF, 4>& corners) {
  const auto by_x = [](const PointF& a, const PointF& b) { return a.x < b.x; };
  const auto by_y = [](const PointF& a, const PointF& b) { return a.y < b.y; };
  const auto [left, right] = std::minmax_element(corners.begin(), corners.end(), by_x);
  const auto [top, bottom] = std::minmax_element(corners.begin(), corners.end(), by_y);
  return RectF{
      .x = left->x, .y = top->y, .width = right->x - left->x, .height = bottom->y - top->y};
}

RectF ImageDisplayable::bounds() const {
  return bounding_box(place_corners(properties(), size_));
}

}  // namespace vnmaki
