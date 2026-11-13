#pragma once

// Save thumbnails (docs/SPEC.md section 8): a small copy of the screen kept in the save file.

#include "engine/render/image.hpp"
#include "engine/render/renderer.hpp"
#include "engine/saves/save_file.hpp"

namespace vnmaki {

/// The width of a thumbnail. An image that is narrower keeps its size.
inline constexpr int kThumbnailWidth = 192;

/// Shrinks `image` to kThumbnailWidth (the height keeps the aspect ratio) by averaging the
/// pixels that fall into each thumbnail pixel. Empty for an image with no pixels or a wrong
/// pixel count.
[[nodiscard]] Thumbnail make_thumbnail(const Image& image);

/// Reads `target` back from the GPU (IRenderer::read_pixels, so between frames) and shrinks it.
/// Throws RenderError.
[[nodiscard]] Thumbnail capture_thumbnail(IRenderer& renderer, TextureId target);

/// The thumbnail of a run with no renderer (headless mode, tests): a plain grey 16:9 rectangle.
[[nodiscard]] Thumbnail stub_thumbnail();

}  // namespace vnmaki
