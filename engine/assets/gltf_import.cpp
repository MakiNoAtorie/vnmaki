#include "engine/assets/gltf_import.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <cgltf.h>

namespace vnmaki {

namespace {

constexpr std::string_view kDataUri = "data:";

struct GltfDeleter {
  void operator()(cgltf_data* data) const { cgltf_free(data); }
};

// cgltf reads the external buffers through the Vfs.
cgltf_result read_from_vfs(const cgltf_memory_options* /*memory*/,
                           const cgltf_file_options* options, const char* path, cgltf_size* size,
                           void** data) {
  const auto* vfs = static_cast<const Vfs*>(options->user_data);
  const std::optional<std::vector<std::byte>> bytes = vfs->read(path);
  if (!bytes) {
    return cgltf_result_file_not_found;
  }
  if (bytes->size() < *size) {  // on entry: the size that the buffer says it has
    return cgltf_result_data_too_short;
  }
  // cgltf frees it with `release` below.
  void* copy = std::malloc(bytes->empty() ? 1 : bytes->size());  // NOLINT(cppcoreguidelines-no-malloc)
  if (copy == nullptr) {
    return cgltf_result_out_of_memory;
  }
  std::copy(bytes->begin(), bytes->end(), static_cast<std::byte*>(copy));
  *size = bytes->size();
  *data = copy;
  return cgltf_result_success;
}

void release_to_vfs(const cgltf_memory_options* /*memory*/, const cgltf_file_options* /*options*/,
                    void* data) {
  std::free(data);  // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
}

std::string_view result_text(cgltf_result result) {
  switch (result) {
    case cgltf_result_data_too_short:
      return "has a buffer or data that is shorter than it says";
    case cgltf_result_unknown_format:
      return "is not a glTF file";
    case cgltf_result_invalid_json:
      return "has invalid JSON";
    case cgltf_result_invalid_gltf:
      return "is not valid glTF 2.0";
    case cgltf_result_legacy_gltf:
      return "is glTF 1.0, not 2.0";
    case cgltf_result_out_of_memory:
      return "is too big";
    case cgltf_result_file_not_found:
    case cgltf_result_io_error:
      return "has a buffer that cannot be read or is too short";
    case cgltf_result_success:
    case cgltf_result_invalid_options:
      break;
  }
  return "cannot be read";
}

// The Vfs path of a `uri` that is relative to the folder of `gltf_path`.
std::string resolve(std::string_view gltf_path, const char* uri) {
  std::string decoded{uri};
  cgltf_decode_uri(decoded.data());  // in place; the text then ends at the first NUL
  decoded.resize(std::strlen(decoded.c_str()));
  const std::size_t slash = gltf_path.rfind('/');
  const std::string folder =
      slash == std::string_view::npos ? std::string{} : std::string{gltf_path.substr(0, slash + 1)};
  return folder + decoded;
}

}  // namespace

std::vector<std::string> validate_gltf(const Vfs& vfs, std::string_view path) {
  const std::string prefix = std::string{path} + ": ";
  const std::optional<std::vector<std::byte>> bytes = vfs.read(path);
  if (!bytes) {
    return {prefix + "not found"};
  }
  cgltf_options options{};
  options.file.read = read_from_vfs;
  options.file.release = release_to_vfs;
  options.file.user_data = const_cast<void*>(static_cast<const void*>(&vfs));  // only read
  cgltf_data* parsed = nullptr;
  const cgltf_result parse_result = cgltf_parse(&options, bytes->data(), bytes->size(), &parsed);
  const std::unique_ptr<cgltf_data, GltfDeleter> data{parsed};
  if (parse_result != cgltf_result_success) {
    return {prefix + std::string{result_text(parse_result)}};
  }

  std::vector<std::string> problems;
  const auto check_uri = [&](const char* kind, std::size_t index, const char* uri) {
    if (uri == nullptr || std::string_view{uri}.starts_with(kDataUri)) {
      return;  // inside the file (the .glb chunk) or in the uri itself
    }
    if (!vfs.exists(resolve(path, uri))) {
      problems.push_back(prefix + kind + " " + std::to_string(index) + " '" + uri + "' not found");
    }
  };
  for (std::size_t i = 0; i < data->buffers_count; ++i) {
    check_uri("buffer", i, data->buffers[i].uri);
  }
  for (std::size_t i = 0; i < data->images_count; ++i) {
    check_uri("texture image", i, data->images[i].uri);
  }
  if (!problems.empty()) {
    return problems;
  }

  const std::string gltf_path{path};
  if (const cgltf_result loaded = cgltf_load_buffers(&options, data.get(), gltf_path.c_str());
      loaded != cgltf_result_success) {
    return {prefix + std::string{result_text(loaded)}};
  }
  if (const cgltf_result valid = cgltf_validate(data.get()); valid != cgltf_result_success) {
    return {prefix + std::string{result_text(valid)}};
  }
  return {};
}

}  // namespace vnmaki
