#pragma once

// Decoded images and the image loader. Images reach the GPU through IRenderer::create_texture
// (engine/render/renderer.hpp).

#include <cstddef>
#include <cstdint>
#include <functional>
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

/// Decodes a whole PNG, JPEG or WebP file (lossless or lossy, no animation). Throws ImageError if
/// the data is not a complete image of one of these kinds or the image is over the size limits.
/// Safe to call from any thread.
[[nodiscard]] Image decode_image(std::span<const std::byte> encoded);

/// A decoder of a file format that is not built in (a plugin's: engine/plugin/plugin.hpp). It
/// returns the image with STRAIGHT alpha, and throws ImageError if the data is bad.
using ImageFormatDecoder = std::function<Image(std::span<const std::byte>)>;

/// Makes `load_image` decode files with `extension` (lower case, with the dot: ".ppm") through
/// `decoder`, and `resolve_image` (engine/scene/scene.hpp) find them after the built-in ones. The
/// table is process-wide and has a lock, because jobs read it; register at start-up. Throws
/// std::invalid_argument for a bad extension, a built-in one or one that is registered.
void register_image_format(std::string extension, ImageFormatDecoder decoder);
/// Removes every registered format (tests).
void clear_image_formats();
/// The registered extensions, sorted.
[[nodiscard]] std::vector<std::string> image_format_extensions();

/// Reads the image at virtual path `path` through `vfs` and decodes it, both on a job of `jobs`.
/// The handle yields the image, or rethrows ImageError if the file is missing or cannot be
/// decoded. `vfs` must outlive the job and must not be mounted to meanwhile.
[[nodiscard]] JobHandle<Image> load_image(const Vfs& vfs, JobSystem& jobs, std::string path);

}  // namespace vnmaki
