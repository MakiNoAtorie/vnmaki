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

#include "engine/render/image.hpp"
#include "engine/render/mesh.hpp"

namespace vnmaki {

class Vfs;

/// The most vertices and indices a model may have in all its meshes, so that a hostile file
/// cannot exhaust memory. A node that uses a mesh again counts the mesh again.
inline constexpr std::size_t kMaxModelVertices = std::size_t{1} << 20;
inline constexpr std::size_t kMaxModelIndices = std::size_t{3} << 20;

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

/// One primitive with the transform of its node applied.
struct ModelMesh {
  std::vector<MeshVertex> vertices{};
  /// Triples of indices into `vertices`.
  std::vector<std::uint32_t> indices{};
  /// An index in ModelData::materials.
  std::size_t material = 0;
};

struct ModelData {
  std::vector<ModelMesh> meshes{};
  /// A primitive without a material uses the last one: white, not metallic, half rough.
  std::vector<ModelMaterial> materials{};
  /// The textures the materials use, decoded (premultiplied, see Image).
  std::vector<Image> images{};
};

/// Reads the .gltf or .glb file at `path` and the buffers and images it names (paths in the file
/// are relative to its folder), all through `vfs`. Meshes of nodes are placed by their node
/// transforms. Skins, animations, cameras and lights are ignored. Only triangle lists are
/// drawn. A missing or malformed file, buffer or image, and a primitive that is not a triangle
/// list, throw ModelError.
[[nodiscard]] ModelData load_model(const Vfs& vfs, std::string_view path);

/// As load_model, from the bytes of the file at `path` (which finds the files it names).
[[nodiscard]] ModelData parse_model(const Vfs& vfs, std::string_view path,
                                    std::span<const std::byte> bytes);

}  // namespace vnmaki
