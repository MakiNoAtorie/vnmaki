#include "engine/platform/viewport.hpp"

#include <algorithm>
#include <cmath>

namespace vnmaki {

Viewport compute_viewport(Size virtual_size, Size window_pixels, ScaleMode mode) {
  if (virtual_size.width <= 0 || virtual_size.height <= 0 || window_pixels.width <= 0 ||
      window_pixels.height <= 0) {
    return {};
  }
  const float scale_x =
      static_cast<float>(window_pixels.width) / static_cast<float>(virtual_size.width);
  const float scale_y =
      static_cast<float>(window_pixels.height) / static_cast<float>(virtual_size.height);
  float scale = std::min(scale_x, scale_y);
  if (mode == ScaleMode::kFill) {
    scale = std::max(scale_x, scale_y);
  } else if (mode == ScaleMode::kInteger && scale >= 1) {
    scale = std::floor(scale);
  }

  const float width = static_cast<float>(virtual_size.width) * scale;
  const float height = static_cast<float>(virtual_size.height) * scale;
  const float x = std::floor((static_cast<float>(window_pixels.width) - width) / 2);
  const float y = std::floor((static_cast<float>(window_pixels.height) - height) / 2);
  return {.rect = {x, y, width, height}, .scale = scale};
}

namespace {

/// Pixels per point along one axis; 1 when either side is empty.
float axis_density(int points, int pixels) {
  if (points <= 0 || pixels <= 0) {
    return 1;
  }
  return static_cast<float>(pixels) / static_cast<float>(points);
}

}  // namespace

float pixel_density(const WindowMetrics& metrics) {
  return axis_density(metrics.points.width, metrics.pixels.width);
}

PointF points_to_pixels(const WindowMetrics& metrics, PointF point) {
  return {.x = point.x * axis_density(metrics.points.width, metrics.pixels.width),
          .y = point.y * axis_density(metrics.points.height, metrics.pixels.height)};
}

PointF virtual_to_window(const Viewport& viewport, PointF virtual_point) {
  return {.x = viewport.rect.x + (virtual_point.x * viewport.scale),
          .y = viewport.rect.y + (virtual_point.y * viewport.scale)};
}

PointF window_to_virtual(const Viewport& viewport, PointF window_point) {
  if (viewport.scale <= 0) {
    return {};
  }
  return {.x = (window_point.x - viewport.rect.x) / viewport.scale,
          .y = (window_point.y - viewport.rect.y) / viewport.scale};
}

}  // namespace vnmaki
