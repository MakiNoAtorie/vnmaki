#include "engine/render/frame_recorder.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vnmaki::render_detail {

void FrameRecorder::reset() {
  vertices_.clear();
  batches_.clear();
  passes_.clear();
  uniform_bytes_.clear();
}

void FrameRecorder::begin_pass(const RenderPass& pass) {
  passes_.push_back(RecordedPass{.pass = pass, .first_batch = batches_.size(), .batch_count = 0});
}

bool FrameRecorder::joins_last_batch(const DrawState& state) const {
  const RecordedPass& pass = passes_.back();
  if (pass.batch_count == 0) {
    return false;
  }
  const Batch& last = batches_.back();
  if (last.texture != state.texture || last.shader != state.shader || last.blend != state.blend) {
    return false;
  }
  const std::span<const std::byte> last_uniforms =
      std::span{uniform_bytes_}.subspan(last.uniform_offset, last.uniform_size);
  return std::ranges::equal(last_uniforms, state.uniforms);
}

void FrameRecorder::add(std::span<const Quad> quads, const DrawState& state) {
  if (quads.empty()) {
    return;
  }
  if (!joins_last_batch(state)) {
    const std::size_t uniform_offset = uniform_bytes_.size();
    uniform_bytes_.insert(uniform_bytes_.end(), state.uniforms.begin(), state.uniforms.end());
    batches_.push_back(Batch{.texture = state.texture,
                             .shader = state.shader,
                             .blend = state.blend,
                             .uniform_offset = uniform_offset,
                             .uniform_size = state.uniforms.size(),
                             .first_quad = quad_count(),
                             .quad_count = 0});
    ++passes_.back().batch_count;
  }
  batches_.back().quad_count += static_cast<std::uint32_t>(quads.size());

  vertices_.reserve(vertices_.size() + (quads.size() * kVerticesPerQuad));
  for (const Quad& quad : quads) {
    const Color& tint = quad.tint;
    const float r = tint.r * tint.a;
    const float g = tint.g * tint.a;
    const float b = tint.b * tint.a;
    const float u0 = quad.uv.x;
    const float v0 = quad.uv.y;
    const float u1 = quad.uv.x + quad.uv.width;
    const float v1 = quad.uv.y + quad.uv.height;
    const std::array<float, kVerticesPerQuad> us{u0, u1, u1, u0};
    const std::array<float, kVerticesPerQuad> vs{v0, v0, v1, v1};
    for (std::size_t corner = 0; corner < kVerticesPerQuad; ++corner) {
      vertices_.push_back(SpriteVertex{.x = quad.corners.at(corner).x,
                                       .y = quad.corners.at(corner).y,
                                       .u = us.at(corner),
                                       .v = vs.at(corner),
                                       .r = r,
                                       .g = g,
                                       .b = b,
                                       .a = tint.a});
    }
  }
}

}  // namespace vnmaki::render_detail
