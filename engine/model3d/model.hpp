#pragma once

// glTF 2.0 models (docs/SPEC.md section 12): the meshes and materials of a .gltf or .glb file,
// read from the Vfs and flattened into what the renderer draws.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "engine/model3d/math3d.hpp"
#include "engine/render/image.hpp"
#include "engine/render/mesh.hpp"

namespace vnmaki {

class Vfs;

/// The most vertices and indices a model may have in all its meshes, so that a hostile file
/// cannot exhaust memory. A node that uses a mesh again counts the mesh again.
inline constexpr std::size_t kMaxModelVertices = std::size_t{1} << 20;
inline constexpr std::size_t kMaxModelIndices = std::size_t{3} << 20;

/// More than this many morph targets on a mesh, joints in a skin, clips, or floats (times and
/// values) in all the clips are an error. The morph deltas of all meshes (vertices times targets)
/// also count against kMaxModelVertices.
inline constexpr std::size_t kMaxMorphTargets = 64;
inline constexpr std::size_t kMaxJoints = 256;
inline constexpr std::size_t kMaxClips = 256;
inline constexpr std::size_t kMaxAnimationFloats = std::size_t{4} << 20;

/// Thrown when a model cannot be read. The message names the file and the reason.
class ModelError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// The metallic-roughness material of a glTF primitive (the part this engine draws).
struct ModelMaterial {
  /// The base colour factor (straight alpha).
  std::array<float, 4> color{1, 1, 1, 1};
  float metallic = 0;
  float roughness = 1;
  /// The index in ModelData::images of the base colour texture, or -1 for none.
  int image = -1;
};

/// The position of one morph target of a mesh: how far each vertex moves at weight 1.
struct MorphTarget {
  std::string name{};  ///< From the mesh's "targetNames", or empty.
  /// One delta for each vertex of the mesh (in the same space as the vertices).
  std::vector<std::array<float, 3>> deltas{};
};

/// The (up to four) joints that move a vertex: indices in ModelSkin::joints and weights that add
/// up to 1.
struct SkinInfluence {
  std::array<std::uint16_t, 4> joints{};
  std::array<float, 4> weights{};
};

/// One primitive. The vertices have the transform of their node applied, except those of a
/// skinned mesh: they are as the file has them and the skin moves them (glTF ignores the node
/// transform of a skinned mesh).
struct ModelMesh {
  std::vector<MeshVertex> vertices{};
  /// Triples of indices into `vertices`.
  std::vector<std::uint32_t> indices{};
  /// An index in ModelData::materials.
  std::size_t material = 0;
  /// The index in ModelData::nodes of the node that holds the mesh. The weights channels of
  /// clips and ModelNode::weights are for this node.
  std::size_t node = 0;
  /// An index in ModelData::skins, or -1 for a mesh that is not skinned. Then `influences` has
  /// one entry for each vertex.
  int skin = -1;
  std::vector<SkinInfluence> influences{};
  std::vector<MorphTarget> morphs{};
};

/// A node of the glTF file: its local transform at rest, and the weights of the morph targets of
/// its mesh at rest.
struct ModelNode {
  /// An index in ModelData::nodes, or -1 for a root.
  int parent = -1;
  Vec3 translation{};
  Quat rotation = kIdentityQuat;
  Vec3 scale{.x = 1, .y = 1, .z = 1};
  /// One weight for each morph target of the node's mesh, or none.
  std::vector<float> weights{};
};

struct ModelSkin {
  /// The nodes (indices in ModelData::nodes) of the joints.
  std::vector<std::size_t> joints{};
  /// One matrix for each joint: bind pose to joint space.
  std::vector<Mat4> inverse_bind{};
};

enum class AnimationPath : std::uint8_t { kTranslation, kRotation, kScale, kWeights };
enum class Interpolation : std::uint8_t { kLinear, kStep, kCubicSpline };

/// One property of one node over time.
struct AnimationChannel {
  std::size_t node = 0;
  AnimationPath path = AnimationPath::kTranslation;
  Interpolation interpolation = Interpolation::kLinear;
  /// Seconds, not falling; at least one.
  std::vector<float> times{};
  /// The number of floats of one value: 3, 4 (a rotation) or the number of morph targets.
  std::size_t width = 0;
  /// `width` floats for each time. A cubic spline has three (in-tangent, value, out-tangent).
  std::vector<float> values{};
};

/// A named clip (a glTF animation).
struct ModelClip {
  std::string name{};
  /// The latest time of its channels, in seconds.
  float duration = 0;
  std::vector<AnimationChannel> channels{};
};

struct ModelData {
  std::vector<ModelMesh> meshes{};
  /// All the nodes of the file, with the order of their indices.
  std::vector<ModelNode> nodes{};
  /// The indices of `nodes` with every parent before its children.
  std::vector<std::size_t> node_order{};
  std::vector<ModelSkin> skins{};
  std::vector<ModelClip> clips{};
  /// A primitive without a material uses the last one: white, not metallic, half rough.
  std::vector<ModelMaterial> materials{};
  /// The textures the materials use, decoded (premultiplied, see Image).
  std::vector<Image> images{};
};

/// Reads the .gltf or .glb file at `path` and the buffers and images it names (paths in the file
/// are relative to its folder), all through `vfs`. Meshes of nodes are placed by their node
/// transforms. Skins (the first four joints of a vertex), morph targets (positions) and clips
/// are read (engine/model3d/animation.hpp plays them); cameras and lights are ignored. Only
/// triangle lists are drawn. A missing or malformed file, buffer or image, and a primitive that is
/// not a triangle list, throw ModelError.
[[nodiscard]] ModelData load_model(const Vfs& vfs, std::string_view path);

/// As load_model, from the bytes of the file at `path` (which finds the files it names).
[[nodiscard]] ModelData parse_model(const Vfs& vfs, std::string_view path,
                                    std::span<const std::byte> bytes);

}  // namespace vnmaki
