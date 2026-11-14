#include "engine/render/renderer.hpp"

namespace vnmaki {

Quad quad_from_rect(RectF destination, RectF uv, Color tint) {
  const float left = destination.x;
  const float top = destination.y;
  const float right = destination.x + destination.width;
  const float bottom = destination.y + destination.height;
  return Quad{.corners = {PointF{.x = left, .y = top}, PointF{.x = right, .y = top},
                          PointF{.x = right, .y = bottom}, PointF{.x = left, .y = bottom}},
              .uv = uv,
              .tint = tint};
}

}  // namespace vnmaki
