// Fuzzes the glTF loader (engine/model3d/model.hpp): the bytes are the .gltf or .glb file, and the
// Vfs has one more file, the texture, so a file that names it can load.
//
// Besides "does not crash": the loader either throws ModelError or returns a model that is safe to
// draw (fuzz/model_properties.hpp), and every clip of it plays to its end without leaving a range.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/model3d/animation.hpp"
#include "engine/model3d/model.hpp"
#include "engine/vfs/vfs.hpp"
#include "fuzz/model_properties.hpp"

namespace {

// A Vfs of one file, "model.bin": the external buffer that a fuzzed file may name.
class BufferMount final : public vnmaki::VfsMount {
 public:
  explicit BufferMount(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {}

  [[nodiscard]] bool is_file(std::string_view path) const override { return path == "model.bin"; }
  [[nodiscard]] std::optional<std::vector<std::byte>> read(std::string_view path) const override {
    return path == "model.bin" ? std::optional{bytes_} : std::nullopt;
  }
  [[nodiscard]] std::optional<std::vector<vnmaki::VfsEntry>> list(
      std::string_view /*directory*/) const override {
    return std::nullopt;
  }

 private:
  std::vector<std::byte> bytes_;
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::byte> bytes = std::as_bytes(std::span<const std::uint8_t>{data, size});
  vnmaki::Vfs vfs;
  // The same bytes serve as the external buffer, so a short file still reaches the accessors.
  vfs.mount(std::make_unique<BufferMount>(std::vector<std::byte>{bytes.begin(), bytes.end()}));
  try {
    const vnmaki::ModelData model = vnmaki::parse_model(vfs, "model.gltf", bytes);
    if (!vnmaki::fuzz::model_is_consistent(model)) {
      std::abort();
    }
    for (const vnmaki::ModelClip& clip : model.clips) {
      vnmaki::ClipPlayer player;
      vnmaki::start_clip(player, clip.name, vnmaki::PlayOptions{.seek = clip.duration});
      const vnmaki::ModelPose pose = vnmaki::evaluate_pose(model, player);
      const auto skins = vnmaki::skin_matrices(model, pose);
      std::vector<vnmaki::MeshVertex> vertices;
      for (const vnmaki::ModelMesh& mesh : model.meshes) {
        vnmaki::pose_vertices(mesh, pose, skins, vertices);
      }
    }
  } catch (const vnmaki::ModelError&) {
    return 0;
  }
  return 0;
}
