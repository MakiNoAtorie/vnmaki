#include "engine/saves/thumbnail.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/render/image.hpp"
#include "engine/render/renderer.hpp"

namespace vnmaki {

namespace {

constexpr std::size_t kChannels = 4;
constexpr int kStubWidth = 16;
constexpr int kStubHeight = 9;
constexpr std::uint8_t kStubGrey = 0x60;

}  // namespace

Thumbnail make_thumbnail(const Image& image) {
  const int source_width = image.size.width;
  const int source_height = image.size.height;
  const auto source_bytes =
      static_cast<std::size_t>(source_width) * static_cast<std::size_t>(source_height) * kChannels;
  if (source_width <= 0 || source_height <= 0 || image.pixels.size() != source_bytes) {
    return {};
  }
  const int width = std::min(source_width, kThumbnailWidth);
  const auto scaled_height =
      (static_cast<std::int64_t>(source_height) * width + source_width / 2) / source_width;
  const int height =
      static_cast<int>(std::clamp<std::int64_t>(scaled_height, 1, kMaxThumbnailSide));

  Thumbnail thumbnail{.width = width, .height = height, .rgba = {}};
  thumbnail.rgba.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                         kChannels);
  for (int y = 0; y < height; ++y) {
    const int y_begin = static_cast<int>(static_cast<std::int64_t>(y) * source_height / height);
    const int y_end = std::max(
        y_begin + 1, static_cast<int>(static_cast<std::int64_t>(y + 1) * source_height / height));
    for (int x = 0; x < width; ++x) {
      const int x_begin = static_cast<int>(static_cast<std::int64_t>(x) * source_width / width);
      const int x_end = std::max(
          x_begin + 1, static_cast<int>(static_cast<std::int64_t>(x + 1) * source_width / width));
      std::array<std::int64_t, kChannels> sums{};
      for (int sy = y_begin; sy < y_end; ++sy) {
        for (int sx = x_begin; sx < x_end; ++sx) {
          const auto offset =
              (static_cast<std::size_t>(sy) * static_cast<std::size_t>(source_width) +
               static_cast<std::size_t>(sx)) *
              kChannels;
          for (std::size_t c = 0; c < kChannels; ++c) {
            sums.at(c) += image.pixels.at(offset + c);
          }
        }
      }
      const std::int64_t count = static_cast<std::int64_t>(y_end - y_begin) * (x_end - x_begin);
      for (const std::int64_t sum : sums) {
        thumbnail.rgba.push_back(static_cast<std::uint8_t>((sum + count / 2) / count));
      }
    }
  }
  return thumbnail;
}

Thumbnail capture_thumbnail(IRenderer& renderer, TextureId target) {
  return make_thumbnail(renderer.read_pixels(target));
}

Thumbnail stub_thumbnail() {
  Thumbnail thumbnail{.width = kStubWidth, .height = kStubHeight, .rgba = {}};
  thumbnail.rgba.reserve(static_cast<std::size_t>(kStubWidth) * kStubHeight * kChannels);
  for (int i = 0; i < kStubWidth * kStubHeight; ++i) {
    thumbnail.rgba.insert(thumbnail.rgba.end(), {kStubGrey, kStubGrey, kStubGrey, UINT8_MAX});
  }
  return thumbnail;
}

}  // namespace vnmaki
