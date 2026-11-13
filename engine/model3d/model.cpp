#include "engine/model3d/model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cgltf.h>

#include "engine/vfs/vfs.hpp"

namespace vnmaki {

namespace {

constexpr std::size_t kMatrixSize = 4 * 4;
constexpr float kTiny = 1e-12F;
// A primitive without a material: white, not metallic, half rough.
constexpr float kDefaultRoughness = 0.5F;

struct CgltfDeleter {
  void operator()(cgltf_data* data) const { cgltf_free(data); }
};

// What the file callbacks of cgltf read through.
struct VfsContext {
  const Vfs* vfs = nullptr;
};

void* allocate(void* /*user*/, cgltf_size size) { return ::operator new(size, std::nothrow); }
void release_memory(void* /*user*/, void* memory) { ::operator delete(memory); }

cgltf_result read_file(const cgltf_memory_options* memory, const cgltf_file_options* files,
                       const char* path, cgltf_size* size, void** data) {
  const auto* context = static_cast<const VfsContext*>(files->user_data);
  const std::optional<std::vector<std::byte>> bytes = context->vfs->read(path);
  if (!bytes.has_value()) {
    return cgltf_result_file_not_found;
  }
  // A buffer may be larger than its byteLength says, never smaller.
  const cgltf_size wanted = *size != 0 ? *size : bytes->size();
  if (bytes->size() < wanted) {
    return cgltf_result_io_error;
  }
  void* copy = memory->alloc_func(memory->user_data, wanted);
  if (copy == nullptr) {
    return cgltf_result_out_of_memory;
  }
  std::memcpy(copy, bytes->data(), wanted);
  *size = wanted;
  *data = copy;
  return cgltf_result_success;
}

void release_file(const cgltf_memory_options* memory, const cgltf_file_options* /*files*/,
                  void* data) {
  memory->free_func(memory->user_data, data);
}

std::string result_text(cgltf_result result) {
  switch (result) {
    case cgltf_result_data_too_short:
      return "the file is too short";
    case cgltf_result_unknown_format:
      return "it is not a glTF file";
    case cgltf_result_invalid_json:
      return "the JSON is not valid";
    case cgltf_result_invalid_gltf:
      return "it is not a valid glTF 2.0 file";
    case cgltf_result_invalid_options:
      return "bad options";
    case cgltf_result_file_not_found:
      return "a file that it names is missing";
    case cgltf_result_io_error:
      return "a file that it names is too short";
    case cgltf_result_out_of_memory:
      return "out of memory";
    case cgltf_result_legacy_gltf:
      return "glTF 1.0 is not supported";
    default:
      return "an unknown error";
  }
}

[[noreturn]] void fail(std::string_view path, const std::string& why) {
  throw ModelError{"model '" + std::string{path} + "': " + why};
}

// The folder of `path` with a trailing "/", or an empty string.
std::string folder_of(std::string_view path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string_view::npos ? std::string{} : std::string{path.substr(0, slash + 1)};
}

using Matrix = std::array<cgltf_float, kMatrixSize>;

// A column-major matrix times a point (w = 1), or a direction (w = 0).
std::array<float, 3> transform(const Matrix& m, const std::array<float, 3>& v, float w) {
  const std::array<float, 4> in{v.at(0), v.at(1), v.at(2), w};
  std::array<float, 3> out{};
  for (std::size_t row = 0; row < 3; ++row) {
    for (std::size_t column = 0; column < 4; ++column) {
      out.at(row) += m.at((column * 4) + row) * in.at(column);
    }
  }
  return out;
}

std::array<float, 3> normalized(const std::array<float, 3>& v,
                                const std::array<float, 3>& fallback) {
  const float squared = (v.at(0) * v.at(0)) + (v.at(1) * v.at(1)) + (v.at(2) * v.at(2));
  if (!(squared > kTiny) || !std::isfinite(squared)) {
    return fallback;
  }
  const float length = std::sqrt(squared);
  return {v.at(0) / length, v.at(1) / length, v.at(2) / length};
}

constexpr std::array<float, 3> kUp{0, 1, 0};

// Normals from the triangles: each vertex gets the sum of the normals of its faces.
void fill_normals(ModelMesh& mesh) {
  std::vector<std::array<float, 3>> sums(mesh.vertices.size());
  for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
    const std::array<std::uint32_t, 3> corner{mesh.indices.at(i), mesh.indices.at(i + 1),
                                              mesh.indices.at(i + 2)};
    const auto& a = mesh.vertices.at(corner.at(0)).position;
    const auto& b = mesh.vertices.at(corner.at(1)).position;
    const auto& c = mesh.vertices.at(corner.at(2)).position;
    const std::array<float, 3> ab{b.at(0) - a.at(0), b.at(1) - a.at(1), b.at(2) - a.at(2)};
    const std::array<float, 3> ac{c.at(0) - a.at(0), c.at(1) - a.at(1), c.at(2) - a.at(2)};
    const std::array<float, 3> face{(ab.at(1) * ac.at(2)) - (ab.at(2) * ac.at(1)),
                                    (ab.at(2) * ac.at(0)) - (ab.at(0) * ac.at(2)),
                                    (ab.at(0) * ac.at(1)) - (ab.at(1) * ac.at(0))};
    for (const std::uint32_t index : corner) {
      for (std::size_t axis = 0; axis < 3; ++axis) {
        sums.at(index).at(axis) += face.at(axis);
      }
    }
  }
  for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
    mesh.vertices.at(i).normal = normalized(sums.at(i), kUp);
  }
}

// The floats of an accessor, or an error if it does not have `count` elements of `width` floats.
std::vector<float> read_floats(const cgltf_accessor& accessor, std::size_t count, std::size_t width,
                               std::string_view path, const char* what) {
  if (accessor.count != count || cgltf_num_components(accessor.type) != width) {
    fail(path, std::string{what} + " does not match the positions");
  }
  std::vector<float> floats(count * width);
  if (cgltf_accessor_unpack_floats(&accessor, floats.data(), floats.size()) != floats.size()) {
    fail(path, std::string{what} + " cannot be read");
  }
  return floats;
}

const cgltf_accessor* attribute_of(const cgltf_primitive& primitive, cgltf_attribute_type type) {
  for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
    const cgltf_attribute& attribute = primitive.attributes[i];
    if (attribute.type == type && attribute.index == 0) {
      return attribute.data;
    }
  }
  return nullptr;
}

class Loader {
 public:
  Loader(const Vfs& vfs, std::string_view path, const cgltf_data& data)
      : vfs_(vfs), path_(path), data_(data) {}

  ModelData load() {
    result_.materials.reserve(data_.materials_count + 1);
    for (cgltf_size i = 0; i < data_.materials_count; ++i) {
      result_.materials.push_back(read_material(data_.materials[i]));
    }
    result_.materials.push_back(ModelMaterial{.roughness = kDefaultRoughness});
    for (cgltf_size i = 0; i < data_.nodes_count; ++i) {
      const cgltf_node& node = data_.nodes[i];
      if (node.mesh == nullptr) {
        continue;
      }
      Matrix world{};
      cgltf_node_transform_world(&node, world.data());
      for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
        add_primitive(node.mesh->primitives[p], world);
      }
    }
    return std::move(result_);
  }

 private:
  ModelMaterial read_material(const cgltf_material& material) {
    ModelMaterial result;
    if (!material.has_pbr_metallic_roughness) {
      return result;
    }
    const cgltf_pbr_metallic_roughness& pbr = material.pbr_metallic_roughness;
    std::copy(std::begin(pbr.base_color_factor), std::end(pbr.base_color_factor),
              result.color.begin());
    result.metallic = pbr.metallic_factor;
    result.roughness = pbr.roughness_factor;
    const cgltf_texture* texture = pbr.base_color_texture.texture;
    if (texture != nullptr) {
      result.image = image_index(texture->image);
    }
    return result;
  }

  // The index of `image` in the result, decoding it the first time.
  int image_index(const cgltf_image* image) {
    if (image == nullptr) {
      fail(path_, "a texture has no image");
    }
    const auto found = decoded_.find(image);
    if (found != decoded_.end()) {
      return found->second;
    }
    std::vector<std::byte> owned;
    std::span<const std::byte> bytes;
    if (image->buffer_view != nullptr) {
      const cgltf_buffer_view& view = *image->buffer_view;
      if (view.buffer->data == nullptr) {
        fail(path_, "an image has no data");
      }
      bytes = std::as_bytes(
          std::span{static_cast<const std::uint8_t*>(view.buffer->data) + view.offset, view.size});
    } else if (image->uri != nullptr && std::string_view{image->uri}.rfind("data:", 0) != 0) {
      std::string uri = image->uri;
      cgltf_decode_uri(uri.data());
      uri.resize(std::strlen(uri.c_str()));
      std::optional<std::vector<std::byte>> file = vfs_.read(folder_of(path_) + uri);
      if (!file.has_value()) {
        fail(path_, "the image '" + uri + "' is missing");
      }
      owned = std::move(*file);
      bytes = owned;
    } else {
      fail(path_, "an image must be a file or part of a .glb file");
    }
    try {
      result_.images.push_back(decode_image(bytes));
    } catch (const ImageError& error) {
      fail(path_, std::string{"a texture cannot be read: "} + error.what());
    }
    const int index = static_cast<int>(result_.images.size()) - 1;
    decoded_.emplace(image, index);
    return index;
  }

  void add_primitive(const cgltf_primitive& primitive, const Matrix& world) {
    if (primitive.type != cgltf_primitive_type_triangles) {
      fail(path_, "only triangle lists can be drawn");
    }
    const cgltf_accessor* positions = attribute_of(primitive, cgltf_attribute_type_position);
    if (positions == nullptr) {
      fail(path_, "a primitive has no positions");
    }
    const std::size_t count = positions->count;
    const std::size_t index_count = primitive.indices != nullptr ? primitive.indices->count : count;
    if (count > kMaxModelVertices || index_count > kMaxModelIndices ||
        total_vertices_ + count > kMaxModelVertices ||
        total_indices_ + index_count > kMaxModelIndices) {
      fail(path_, "the model is too big");
    }
    if (index_count % 3 != 0 || count == 0) {
      fail(path_, "a primitive does not have whole triangles");
    }
    total_vertices_ += count;
    total_indices_ += index_count;

    const std::vector<float> position_values =
        read_floats(*positions, count, 3, path_, "positions");
    std::vector<float> normal_values;
    if (const cgltf_accessor* normals = attribute_of(primitive, cgltf_attribute_type_normal)) {
      normal_values = read_floats(*normals, count, 3, path_, "normals");
    }
    std::vector<float> uv_values;
    if (const cgltf_accessor* uvs = attribute_of(primitive, cgltf_attribute_type_texcoord)) {
      uv_values = read_floats(*uvs, count, 2, path_, "texture coordinates");
    }

    ModelMesh mesh;
    mesh.vertices.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
      MeshVertex& vertex = mesh.vertices.at(i);
      const std::array<float, 3> position{position_values.at(i * 3),
                                          position_values.at((i * 3) + 1),
                                          position_values.at((i * 3) + 2)};
      vertex.position = transform(world, position, 1.0F);
      if (!normal_values.empty()) {
        const std::array<float, 3> normal{normal_values.at(i * 3), normal_values.at((i * 3) + 1),
                                          normal_values.at((i * 3) + 2)};
        vertex.normal = normalized(transform(world, normal, 0.0F), kUp);
      }
      if (!uv_values.empty()) {
        vertex.uv = {uv_values.at(i * 2), uv_values.at((i * 2) + 1)};
      }
    }
    mesh.indices.reserve(index_count);
    for (std::size_t i = 0; i < index_count; ++i) {
      const std::size_t index =
          primitive.indices != nullptr ? cgltf_accessor_read_index(primitive.indices, i) : i;
      if (index >= count) {
        fail(path_, "an index is past the last vertex");
      }
      mesh.indices.push_back(static_cast<std::uint32_t>(index));
    }
    if (normal_values.empty()) {
      fill_normals(mesh);
    }
    mesh.material = primitive.material != nullptr
                        ? static_cast<std::size_t>(primitive.material - data_.materials)
                        : result_.materials.size() - 1;
    result_.meshes.push_back(std::move(mesh));
  }

  const Vfs& vfs_;
  std::string_view path_;
  const cgltf_data& data_;
  ModelData result_;
  std::map<const cgltf_image*, int> decoded_;
  std::size_t total_vertices_ = 0;
  std::size_t total_indices_ = 0;
};

}  // namespace

ModelData parse_model(const Vfs& vfs, std::string_view path, std::span<const std::byte> bytes) {
  const VfsContext context{.vfs = &vfs};
  cgltf_options options{};
  options.memory.alloc_func = &allocate;
  options.memory.free_func = &release_memory;
  options.file.read = &read_file;
  options.file.release = &release_file;
  options.file.user_data = const_cast<VfsContext*>(&context);
  cgltf_data* raw = nullptr;
  if (const cgltf_result result = cgltf_parse(&options, bytes.data(), bytes.size(), &raw);
      result != cgltf_result_success) {
    fail(path, result_text(result));
  }
  const std::unique_ptr<cgltf_data, CgltfDeleter> data{raw};
  const std::string path_text{path};
  if (const cgltf_result result = cgltf_load_buffers(&options, raw, path_text.c_str());
      result != cgltf_result_success) {
    fail(path, result_text(result));
  }
  if (const cgltf_result result = cgltf_validate(raw); result != cgltf_result_success) {
    fail(path, result_text(result));
  }
  return Loader{vfs, path, *raw}.load();
}

ModelData load_model(const Vfs& vfs, std::string_view path) {
  const std::optional<std::vector<std::byte>> bytes = vfs.read(path);
  if (!bytes.has_value()) {
    fail(path, "the file is missing");
  }
  return parse_model(vfs, path, *bytes);
}

}  // namespace vnmaki
