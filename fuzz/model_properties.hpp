#pragma once

// Properties of a loaded model that must hold for any input, shared by fuzz/model_fuzzer.cpp and
// tests/unit/model3d_test.cpp (which also runs them on mutated copies of the fixtures).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "engine/model3d/model.hpp"

namespace vnmaki::fuzz {

constexpr float kUnitLengthTolerance = 1e-3F;

/// True if the nodes, skins, morph targets and clips of `model` only name what exists, so that
/// engine/model3d/animation.hpp can play them without checks.
[[nodiscard]] inline bool rig_is_consistent(const ModelData& model) {
  const std::size_t node_count = model.nodes.size();
  if (model.node_order.size() != node_count) {
    return false;
  }
  for (const std::size_t index : model.node_order) {
    if (index >= node_count) {
      return false;
    }
  }
  for (const ModelNode& node : model.nodes) {
    if (node.parent >= static_cast<int>(node_count)) {
      return false;
    }
  }
  for (const ModelSkin& skin : model.skins) {
    if (skin.inverse_bind.size() != skin.joints.size() || skin.joints.size() > kMaxJoints ||
        std::ranges::any_of(skin.joints,
                            [node_count](std::size_t joint) { return joint >= node_count; })) {
      return false;
    }
  }
  for (const ModelMesh& mesh : model.meshes) {
    if (mesh.node >= node_count || mesh.morphs.size() > kMaxMorphTargets ||
        mesh.skin >= static_cast<int>(model.skins.size()) ||
        mesh.influences.size() != (mesh.skin >= 0 ? mesh.vertices.size() : 0)) {
      return false;
    }
    for (const MorphTarget& target : mesh.morphs) {
      if (target.deltas.size() != mesh.vertices.size()) {
        return false;
      }
    }
    if (mesh.skin >= 0) {
      const std::size_t joints = model.skins.at(static_cast<std::size_t>(mesh.skin)).joints.size();
      for (const SkinInfluence& influence : mesh.influences) {
        float sum = 0;
        for (std::size_t k = 0; k < influence.joints.size(); ++k) {
          sum += influence.weights.at(k);
          if (influence.weights.at(k) > 0 && influence.joints.at(k) >= joints) {
            return false;
          }
        }
        if (!std::isfinite(sum) || std::abs(sum - 1.0F) > kUnitLengthTolerance) {
          return false;
        }
      }
    }
  }
  for (const ModelClip& clip : model.clips) {
    for (const AnimationChannel& channel : clip.channels) {
      const std::size_t per_key =
          channel.width * (channel.interpolation == Interpolation::kCubicSpline ? 3 : 1);
      if (channel.node >= node_count || channel.width == 0 || channel.times.empty() ||
          channel.values.size() != channel.times.size() * per_key ||
          !std::ranges::is_sorted(channel.times) || channel.times.back() > clip.duration) {
        return false;
      }
    }
  }
  return true;
}

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
  return rig_is_consistent(model);
}

}  // namespace vnmaki::fuzz
