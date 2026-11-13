#include "engine/scene/camera.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

namespace vnmaki {

namespace {

constexpr float kDegreesPerHalfTurn = 180.0F;
constexpr float kHalf = 0.5F;

// The shake noise: a hash of the lattice point (one per 1/kShakeRate seconds) and the axis,
// smoothly interpolated. tools/generators/camera_fixtures.py repeats it for the GPU goldens.
constexpr double kShakeRate = 24.0;
constexpr std::uint32_t kHashPoint = 0x9E3779B1U;
constexpr std::uint32_t kHashAxis = 0x85EBCA6BU;
constexpr std::uint32_t kHashFirst = 0x2C1B3C6DU;
constexpr std::uint32_t kHashSecond = 0x297A2D39U;
constexpr unsigned kShiftFirst = 15;
constexpr unsigned kShiftSecond = 12;
constexpr double kHashMax = 4'294'967'295.0;

// A number in [-1, 1] for a lattice point and an axis.
double lattice_value(std::int64_t point, std::uint32_t axis) {
  auto hash = static_cast<std::uint32_t>(point) * kHashPoint + (axis + 1) * kHashAxis;
  hash ^= hash >> kShiftFirst;
  hash *= kHashFirst;
  hash ^= hash >> kShiftSecond;
  hash *= kHashSecond;
  hash ^= hash >> kShiftFirst;
  return static_cast<double>(hash) / kHashMax * 2 - 1;
}

double shake_noise(double seconds, std::uint32_t axis) {
  const double position = seconds * kShakeRate;
  const double whole = std::floor(position);
  const double fraction = position - whole;
  const double smooth = fraction * fraction * (3 - 2 * fraction);
  const auto point = static_cast<std::int64_t>(whole);
  return std::lerp(lattice_value(point, axis), lattice_value(point + 1, axis), smooth);
}

}  // namespace

bool CameraState::is_default() const {
  return pose.position == PointF{} && pose.scale.x == 1.0F && pose.rotation == 0.0F &&
         parallax == CameraState{}.parallax;
}

PointF shake_offset(const Shake& shake) {
  if (shake.duration <= 0 || shake.done()) {
    return {};
  }
  const double amplitude =
      static_cast<double>(shake.strength) * (1 - shake.elapsed / shake.duration);
  return PointF{.x = static_cast<float>(amplitude * shake_noise(shake.elapsed, 0)),
                .y = static_cast<float>(amplitude * shake_noise(shake.elapsed, 1))};
}

CameraView::CameraView(const CameraState& camera, PointF shake, Size virtual_size,
                       std::size_t layer) {
  const DisplayProperties& pose = camera.pose;
  const float factor = camera.parallax.at(layer);
  center_ = {.x = static_cast<float>(virtual_size.width) * kHalf,
             .y = static_cast<float>(virtual_size.height) * kHalf};
  pan_ = {.x = (pose.position.x + shake.x) * factor, .y = (pose.position.y + shake.y) * factor};
  zoom_ = std::max(pose.scale.x, kMinZoom);
  const float radians = pose.rotation * std::numbers::pi_v<float> / kDegreesPerHalfTurn;
  cosine_ = std::cos(radians);
  sine_ = std::sin(radians);
  identity_ = pan_ == PointF{} && zoom_ == 1.0F && pose.rotation == 0.0F;
}

PointF CameraView::apply(PointF point) const {
  if (identity_) {
    return point;
  }
  const float x = point.x - center_.x - pan_.x;
  const float y = point.y - center_.y - pan_.y;
  return PointF{.x = center_.x + (x * cosine_ - y * sine_) * zoom_,
                .y = center_.y + (x * sine_ + y * cosine_) * zoom_};
}

void CameraRenderer::draw(std::span<const Quad> quads, const DrawState& state) {
  moved_.assign(quads.begin(), quads.end());
  for (Quad& quad : moved_) {
    for (PointF& corner : quad.corners) {
      corner = view_.apply(corner);
    }
  }
  inner_.draw(moved_, state);
}

}  // namespace vnmaki
