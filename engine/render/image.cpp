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
#include <webp/decode.h>

#include "engine/core/load_timing.hpp"
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

constexpr std::string_view kRiffTag = "RIFF";
constexpr std::string_view kWebpTag = "WEBP";
constexpr std::size_t kWebpTagOffset = 8;
constexpr std::size_t kWebpHeaderSize = 12;
constexpr const char* kNotAnImage = "not a PNG, JPEG or WebP image";

struct WebpFree {
  void operator()(std::uint8_t* pixels) const { WebPFree(pixels); }
};

bool has_tag(std::span<const std::byte> data, std::size_t offset, std::string_view tag) {
  for (std::size_t index = 0; index < tag.size(); ++index) {
    if (static_cast<char>(data[offset + index]) != tag[index]) {
      return false;
    }
  }
  return true;
}

bool is_webp(std::span<const std::byte> data) {
  return data.size() >= kWebpHeaderSize && has_tag(data, 0, kRiffTag) &&
         has_tag(data, kWebpTagOffset, kWebpTag);
}

void check_size(int width, int height) {
  if (width > kMaxImageDimension || height > kMaxImageDimension ||
      static_cast<std::int64_t>(width) * height > kMaxImagePixels) {
    throw ImageError{"image is too large (" + std::to_string(width) + "x" + std::to_string(height) +
                     "; the limit is " + std::to_string(kMaxImageDimension) +
                     " pixels a side and " + std::to_string(kMaxImagePixels) + " pixels in all)"};
  }
}

/// Fills image.size and image.pixels with straight (not premultiplied) RGBA.
void decode_webp(std::span<const std::byte> encoded, Image& image) {
  // libwebp reads unsigned chars; std::byte storage may be read through them.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* data = reinterpret_cast<const std::uint8_t*>(encoded.data());
  int width = 0;
  int height = 0;
  if (WebPGetInfo(data, encoded.size(), &width, &height) == 0) {
    throw ImageError{kNotAnImage};
  }
  check_size(width, height);
  const std::unique_ptr<std::uint8_t, WebpFree> decoded{
      WebPDecodeRGBA(data, encoded.size(), &width, &height)};
  if (!decoded || width <= 0 || height <= 0) {
    throw ImageError{"corrupt or truncated image data"};
  }
  image.size = Size{.width = width, .height = height};
  const auto byte_count =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * kChannels;
  const std::span<const std::uint8_t> source{decoded.get(), byte_count};
  image.pixels.assign(source.begin(), source.end());
}

void decode_stb(std::span<const std::byte> encoded, Image& image) {
  if (encoded.size() > static_cast<std::size_t>(INT_MAX)) {
    throw ImageError{kNotAnImage};
  }
  // stb_image reads unsigned chars; std::byte storage may be read through them.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* data = reinterpret_cast<const stbi_uc*>(encoded.data());
  const int length = static_cast<int>(encoded.size());

  int width = 0;
  int height = 0;
  int file_channels = 0;
  if (stbi_info_from_memory(data, length, &width, &height, &file_channels) == 0) {
    throw ImageError{kNotAnImage};
  }
  check_size(width, height);

  const std::unique_ptr<stbi_uc, StbiFree> decoded{
      stbi_load_from_memory(data, length, &width, &height, &file_channels, kChannels)};
  if (!decoded || width <= 0 || height <= 0) {
    throw ImageError{"corrupt or truncated image data"};
  }
  image.size = Size{.width = width, .height = height};
  const auto byte_count =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * kChannels;
  const std::span<const stbi_uc> source{decoded.get(), byte_count};
  image.pixels.assign(source.begin(), source.end());
}

}  // namespace

Image decode_image(std::span<const std::byte> encoded) {
  if (encoded.empty()) {
    throw ImageError{kNotAnImage};
  }
  Image image;
  if (is_webp(encoded)) {
    decode_webp(encoded, image);
  } else {
    decode_stb(encoded, image);
  }
  for (std::size_t offset = 0; offset < image.pixels.size(); offset += kChannels) {
    const std::uint8_t alpha = image.pixels[offset + 3];
    for (std::size_t channel = 0; channel < 3; ++channel) {
      image.pixels[offset + channel] = premultiply(image.pixels[offset + channel], alpha);
    }
  }
  return image;
}

JobHandle<Image> load_image(const Vfs& vfs, JobSystem& jobs, std::string path) {
  return jobs.submit([&vfs, path = std::move(path)]() -> Image {
    const LoadTimer timer{path};
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
