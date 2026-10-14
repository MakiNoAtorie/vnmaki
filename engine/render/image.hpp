#pragma once

// Decoded images and the image loader. Images reach the GPU through IRenderer::create_texture
// (engine/render/renderer.hpp).

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"

namespace vnmaki {

class Vfs;

/// The largest width or height the decoder accepts.
inline constexpr int kMaxImageDimension = 16384;
/// The most pixels the decoder accepts (64 Mi pixels: 256 MiB of RGBA). Images over either limit
/// are rejected from their header, before any pixel memory is allocated, so a hostile file
/// cannot exhaust memory.
inline constexpr std::int64_t kMaxImagePixels = std::int64_t{1} << 26;

/// An image in memory: 8-bit RGBA, rows from top to bottom, with PREMULTIPLIED alpha (each
/// colour channel already scaled by alpha), which is what the renderer blends with.
struct Image {
  Size size{};
  /// size.width * size.height * 4 bytes.
  std::vector<std::uint8_t> pixels;
};

/// Thrown when an image cannot be read or decoded. The message names the reason (and the path,
/// for load_image).
class ImageError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Decodes a whole PNG or JPEG file. Throws ImageError if the data is not a complete PNG or JPEG
/// or the image is larger than kMaxImageDimension either way. Safe to call from any thread.
[[nodiscard]] Image decode_image(std::span<const std::byte> encoded);

/// Reads the image at virtual path `path` through `vfs` and decodes it, both on a job of `jobs`.
/// The handle yields the image, or rethrows ImageError if the file is missing or cannot be
/// decoded. `vfs` must outlive the job and must not be mounted to meanwhile.
[[nodiscard]] JobHandle<Image> load_image(const Vfs& vfs, JobSystem& jobs, std::string path);

}  // namespace vnmaki
