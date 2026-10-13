// Fuzzes the image decoder (engine/render/image.hpp): stb_image's PNG and JPEG readers plus the
// engine's size limits and alpha premultiplication. Images ship with games and mods, so they are
// untrusted input.
//
// Besides "does not crash" (stb_image itself is built with the same sanitizers): bad data is
// reported only as ImageError; a decoded image is within the size limits, has exactly
// width * height RGBA pixels, and is properly premultiplied (no colour channel exceeds alpha).

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "engine/render/image.hpp"

namespace {

void require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

void check_image(const vnmaki::Image& image) {
  const int width = image.size.width;
  const int height = image.size.height;
  require(width > 0 && height > 0);
  require(width <= vnmaki::kMaxImageDimension && height <= vnmaki::kMaxImageDimension);
  require(std::int64_t{width} * height <= vnmaki::kMaxImagePixels);
  constexpr std::size_t kChannels = 4;
  require(image.pixels.size() ==
          static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * kChannels);
  for (std::size_t offset = 0; offset < image.pixels.size(); offset += kChannels) {
    const std::uint8_t alpha = image.pixels[offset + 3];
    require(image.pixels[offset] <= alpha && image.pixels[offset + 1] <= alpha &&
            image.pixels[offset + 2] <= alpha);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::byte> bytes = std::as_bytes(std::span{data, size});
  vnmaki::Image image;
  try {
    image = vnmaki::decode_image(bytes);
  } catch (const vnmaki::ImageError&) {
    return 0;  // Rejecting bad data is the expected outcome for most inputs.
  }
  check_image(image);
  return 0;
}
