#pragma once

// Internal to renderer backends: records a frame's passes and quads, batching consecutive quads
// that share a draw state, so a backend can upload all vertices at once and issue one draw call
// per batch.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/render/renderer.hpp"

namespace vnmaki::render_detail {

/// One vertex as shaders/sprite.vert.hlsl reads it.
struct SpriteVertex {
  float x = 0;  ///< Target pixels.
  float y = 0;
  float u = 0;
  float v = 0;
  float r = 0;  ///< Tint, premultiplied.
  float g = 0;
  float b = 0;
  float a = 0;
};

inline constexpr std::uint32_t kVerticesPerQuad = 4;
inline constexpr std::uint32_t kIndicesPerQuad = 6;
/// Vertex indices of one quad's two triangles (corners top-left, top-right, bottom-right,
/// bottom-left).
inline constexpr std::array<std::uint32_t, kIndicesPerQuad> kQuadIndices{0, 1, 2, 2, 3, 0};

/// Quads sharing a draw state, drawn with one draw call.
struct Batch {
  TextureId texture{};
  ShaderId shader{};
  BlendMode blend = BlendMode::kAlpha;
  /// The batch's uniform bytes: a range of FrameRecorder::uniform_bytes().
  std::size_t uniform_offset = 0;
  std::size_t uniform_size = 0;
  /// The batch's quads: a range of FrameRecorder::vertices(), kVerticesPerQuad per quad.
  std::uint32_t first_quad = 0;
  std::uint32_t quad_count = 0;
};

struct RecordedPass {
  RenderPass pass;
  /// The pass's batches: a range of FrameRecorder::batches().
  std::size_t first_batch = 0;
  std::size_t batch_count = 0;
};

class FrameRecorder {
 public:
  /// Forgets everything recorded (keeps the memory for the next frame).
  void reset();

  void begin_pass(const RenderPass& pass);
  [[nodiscard]] bool in_pass() const { return !passes_.empty(); }
  /// The pass being recorded. Requires in_pass().
  [[nodiscard]] const RenderPass& current_pass() const { return passes_.back().pass; }

  /// Appends quads to the current pass (requires in_pass()). They join the pass's last batch when
  /// the draw state (texture, shader, blend mode and uniform bytes) is the same.
  void add(std::span<const Quad> quads, const DrawState& state);

  [[nodiscard]] std::span<const SpriteVertex> vertices() const { return vertices_; }
  [[nodiscard]] std::span<const Batch> batches() const { return batches_; }
  [[nodiscard]] std::span<const RecordedPass> passes() const { return passes_; }
  [[nodiscard]] std::span<const std::byte> uniform_bytes() const { return uniform_bytes_; }
  [[nodiscard]] std::uint32_t quad_count() const {
    return static_cast<std::uint32_t>(vertices_.size() / kVerticesPerQuad);
  }

 private:
  [[nodiscard]] bool joins_last_batch(const DrawState& state) const;

  std::vector<SpriteVertex> vertices_;
  std::vector<Batch> batches_;
  std::vector<RecordedPass> passes_;
  std::vector<std::byte> uniform_bytes_;
};

}  // namespace vnmaki::render_detail
