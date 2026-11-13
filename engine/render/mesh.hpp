#pragma once

// Triangle meshes for 3D scenes (docs/SPEC.md section 12): the vertex format and the matrices.
// A pass with a depth buffer draws them (MeshDraw and IRenderer::draw_mesh in renderer.hpp).

#include <array>
#include <cstdint>

namespace vnmaki {

/// Names a mesh a renderer owns. The zero value names none.
struct MeshId {
  std::uint32_t value = 0;

  bool operator==(const MeshId&) const = default;
};

/// One mesh vertex as shaders/mesh.vert.hlsl reads it: 32 bytes.
struct MeshVertex {
  std::array<float, 3> position{};
  std::array<float, 3> normal{};
  /// Texture coordinates, (0, 0) at the texture's top-left corner.
  std::array<float, 2> uv{};
};

/// A 4x4 matrix, row-major: element (row, column) is [row * 4 + column]. Shaders multiply it with
/// a column vector (`mul(m, v)`).
using Mat4 = std::array<float, 16>;

}  // namespace vnmaki
