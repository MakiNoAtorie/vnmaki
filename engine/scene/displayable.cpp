#include "engine/scene/displayable.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace vnmaki {

namespace {

constexpr float kDegreesPerHalfTurn = 180.0F;

// Where the corners of a `size` box land (top-left, top-right, bottom-right, bottom-left).
std::array<PointF, 4> place_corners(const DisplayProperties& properties, Size size) {
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
  return {place(0, 0), place(1, 0), place(1, 1), place(0, 1)};
}

}  // namespace

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
      DrawState{.texture = texture_});
}

RectF ImageDisplayable::bounds() const {
  const std::array<PointF, 4> corners = place_corners(properties(), size_);
  const auto by_x = [](const PointF& a, const PointF& b) { return a.x < b.x; };
  const auto by_y = [](const PointF& a, const PointF& b) { return a.y < b.y; };
  const auto [left, right] = std::minmax_element(corners.begin(), corners.end(), by_x);
  const auto [top, bottom] = std::minmax_element(corners.begin(), corners.end(), by_y);
  return RectF{
      .x = left->x, .y = top->y, .width = right->x - left->x, .height = bottom->y - top->y};
}

}  // namespace vnmaki
