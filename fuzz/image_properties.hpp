#pragma once

// What must hold when decode_image (engine/render/image.hpp) reads any byte string. The fuzz
// targets fuzz/image_fuzzer.cpp and fuzz/webp_fuzzer.cpp and a unit test
// (tests/unit/image_test.cpp) all run it, so the check itself is tested on compilers that build no
// fuzzers.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "engine/render/image.hpp"

namespace vnmaki::fuzz {

inline void require_image(bool condition) {
  if (!condition) {
    std::abort();
  }
}

/// Calls std::abort() unless decoding gives an ImageError (the only error allowed) or an image
/// that is within the size limits, has exactly width * height RGBA pixels, and is premultiplied
/// (no colour channel exceeds alpha).
inline void check_image_properties(std::span<const std::byte> encoded) {
  Image image;
  try {
    image = decode_image(encoded);
  } catch (const ImageError&) {
    return;  // Rejecting bad data is the expected outcome for most inputs.
  }
  const int width = image.size.width;
  const int height = image.size.height;
  require_image(width > 0 && height > 0);
  require_image(width <= kMaxImageDimension && height <= kMaxImageDimension);
  require_image(std::int64_t{width} * height <= kMaxImagePixels);
  constexpr std::size_t kChannels = 4;
  require_image(image.pixels.size() ==
                static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * kChannels);
  for (std::size_t offset = 0; offset < image.pixels.size(); offset += kChannels) {
    const std::uint8_t alpha = image.pixels[offset + 3];
    require_image(image.pixels[offset] <= alpha && image.pixels[offset + 1] <= alpha &&
                  image.pixels[offset + 2] <= alpha);
  }
}

}  // namespace vnmaki::fuzz
