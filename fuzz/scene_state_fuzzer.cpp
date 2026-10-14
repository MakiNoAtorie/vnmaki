// Fuzzes Scene::restore (engine/scene/scene.hpp), which reads scene snapshots: they reach it
// from save files.
//
// Besides "does not crash": restore either throws std::invalid_argument or takes the state, and
// a state it took captures to bytes that restore to the same capture again.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <vector>

#include "engine/platform/geometry.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"

namespace {

constexpr vnmaki::Size kVirtualSize{.width = 1280, .height = 720};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  vnmaki::Vfs vfs;
  vnmaki::Scene scene{vfs, kVirtualSize};
  try {
    scene.restore(std::as_bytes(std::span<const std::uint8_t>{data, size}));
  } catch (const std::invalid_argument&) {
    return 0;
  }
  const std::vector<std::byte> captured = scene.capture();
  vnmaki::Scene again{vfs, kVirtualSize};
  again.restore(captured);
  if (again.capture() != captured) {
    std::abort();
  }
  return 0;
}
