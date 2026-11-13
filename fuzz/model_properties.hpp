#pragma once

// Properties of a loaded model that must hold for any input, shared by fuzz/model_fuzzer.cpp and
// tests/unit/model3d_test.cpp (which also runs them on mutated copies of the fixtures).

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "engine/model3d/model.hpp"

namespace vnmaki::fuzz {

constexpr float kUnitLengthTolerance = 1e-3F;

/// True if `model` is safe to hand to the renderer: whole triangles with indices in range, a
/// material for each mesh, unit normals, and the size limits kept.
[[nodiscard]] inline bool model_is_consistent(const ModelData& model) {
  std::size_t vertices = 0;
  std::size_t indices = 0;
  for (const ModelMesh& mesh : model.meshes) {
    vertices += mesh.vertices.size();
    indices += mesh.indices.size();
    if (mesh.vertices.empty() || mesh.indices.empty() || mesh.indices.size() % 3 != 0 ||
        mesh.material >= model.materials.size()) {
      return false;
    }
    for (const std::uint32_t index : mesh.indices) {
      if (index >= mesh.vertices.size()) {
        return false;
      }
    }
    for (const MeshVertex& vertex : mesh.vertices) {
      const float squared = (vertex.normal.at(0) * vertex.normal.at(0)) +
                            (vertex.normal.at(1) * vertex.normal.at(1)) +
                            (vertex.normal.at(2) * vertex.normal.at(2));
      if (!std::isfinite(squared) || std::abs(squared - 1.0F) > kUnitLengthTolerance) {
        return false;
      }
    }
  }
  if (vertices > kMaxModelVertices || indices > kMaxModelIndices) {
    return false;
  }
  for (const ModelMaterial& material : model.materials) {
    if (material.image >= static_cast<int>(model.images.size())) {
      return false;
    }
  }
  return true;
}

}  // namespace vnmaki::fuzz
