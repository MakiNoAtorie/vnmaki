// Fuzzes the image decoder (engine/render/image.hpp): stb_image's PNG and JPEG readers, libwebp,
// and the engine's size limits and alpha premultiplication. Images ship with games and mods, so
// they are untrusted input.
//
// Besides "does not crash" (stb_image itself is built with the same sanitizers): bad data is
// reported only as ImageError, and a decoded image keeps the properties in
// fuzz/image_properties.hpp. fuzz/webp_fuzzer.cpp reaches the WebP decoder more often.

#include <cstddef>
#include <cstdint>
#include <span>

#include "fuzz/image_properties.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  vnmaki::fuzz::check_image_properties(std::as_bytes(std::span{data, size}));
  return 0;
}
