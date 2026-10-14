#include "engine/render/image.hpp"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <stb_image.h>

#include "engine/vfs/vfs.hpp"

namespace vnmaki {

namespace {

constexpr int kChannels = 4;
constexpr unsigned kMaxChannelValue = 255;

struct StbiFree {
  void operator()(stbi_uc* pixels) const { stbi_image_free(pixels); }
};

/// Scales a colour channel by alpha, rounding to nearest.
std::uint8_t premultiply(std::uint8_t channel, std::uint8_t alpha) {
  return static_cast<std::uint8_t>(((unsigned{channel} * alpha) + (kMaxChannelValue / 2)) /
                                   kMaxChannelValue);
}

}  // namespace

Image decode_image(std::span<const std::byte> encoded) {
  if (encoded.empty() || encoded.size() > static_cast<std::size_t>(INT_MAX)) {
    throw ImageError{"not a PNG or JPEG image"};
  }
  // stb_image reads unsigned chars; std::byte storage may be read through them.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* data = reinterpret_cast<const stbi_uc*>(encoded.data());
  const int length = static_cast<int>(encoded.size());

  int width = 0;
  int height = 0;
  int file_channels = 0;
  if (stbi_info_from_memory(data, length, &width, &height, &file_channels) == 0) {
    throw ImageError{"not a PNG or JPEG image"};
  }
  if (width > kMaxImageDimension || height > kMaxImageDimension ||
      static_cast<std::int64_t>(width) * height > kMaxImagePixels) {
    throw ImageError{"image is too large (" + std::to_string(width) + "x" + std::to_string(height) +
                     "; the limit is " + std::to_string(kMaxImageDimension) +
                     " pixels a side and " + std::to_string(kMaxImagePixels) + " pixels in all)"};
  }

  const std::unique_ptr<stbi_uc, StbiFree> decoded{
      stbi_load_from_memory(data, length, &width, &height, &file_channels, kChannels)};
  if (!decoded || width <= 0 || height <= 0) {
    throw ImageError{"corrupt or truncated image data"};
  }

  Image image;
  image.size = Size{.width = width, .height = height};
  const auto byte_count =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * kChannels;
  const std::span<const stbi_uc> source{decoded.get(), byte_count};
  image.pixels.assign(source.begin(), source.end());
  for (std::size_t offset = 0; offset < byte_count; offset += kChannels) {
    const std::uint8_t alpha = image.pixels[offset + 3];
    for (std::size_t channel = 0; channel < 3; ++channel) {
      image.pixels[offset + channel] = premultiply(image.pixels[offset + channel], alpha);
    }
  }
  return image;
}

JobHandle<Image> load_image(const Vfs& vfs, JobSystem& jobs, std::string path) {
  return jobs.submit([&vfs, path = std::move(path)]() -> Image {
    const std::optional<std::vector<std::byte>> encoded = vfs.read(path);
    if (!encoded) {
      throw ImageError{"cannot read image '" + path + "'"};
    }
    try {
      return decode_image(*encoded);
    } catch (const ImageError& error) {
      throw ImageError{"cannot decode image '" + path + "': " + error.what()};
    }
  });
}

}  // namespace vnmaki
