// Fuzzes Audio::restore (engine/audio/audio.hpp), which reads audio snapshots: they reach it from
// save files.
//
// Besides "does not crash": restore either throws std::invalid_argument or takes the state, and
// a state it took captures to bytes that restore to the same capture again.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <vector>

#include "engine/audio/audio.hpp"
#include "engine/vfs/vfs.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const vnmaki::Vfs vfs;
  vnmaki::Audio audio{vfs};
  try {
    audio.restore(std::as_bytes(std::span<const std::uint8_t>{data, size}));
  } catch (const std::invalid_argument&) {
    return 0;
  }
  const std::vector<std::byte> captured = audio.capture();
  vnmaki::Audio again{vfs};
  again.restore(captured);
  if (again.capture() != captured) {
    std::abort();
  }
  return 0;
}
