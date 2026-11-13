// Fuzzes the `.vnpak` reader and writer (engine/vfs/pak_mount.hpp). A pak is a file that a player
// gets from outside, and a patch pak is added by the user, so it is untrusted input. The
// properties are in pak_properties.hpp.

#include <cstddef>
#include <cstdint>
#include <span>

#include "fuzz/pak_properties.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(data), size};
  vnmaki::fuzz::check_pak_read_properties(bytes);
  vnmaki::fuzz::check_pak_round_trip_properties(bytes);
  return 0;
}
