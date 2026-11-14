#pragma once

#include <cstdint>

#include "engine/platform/geometry.hpp"

namespace vnmaki {

/// How the virtual canvas is scaled into the window (docs/SPEC.md section 9).
enum class ScaleMode : std::uint8_t {
  /// Largest uniform scale that shows the whole canvas; bars fill the spare space.
  kFit,
  /// Smallest uniform scale that covers the whole window; the canvas overflows and is cropped.
  kFill,
  /// Largest whole-number scale that shows the whole canvas (pixel art stays crisp). Falls back
  /// to kFit when the window is smaller than the canvas.
  kInteger,
};

/// A window's size. Points are the OS's window coordinates (what the pointer reports); pixels
/// are what the GPU draws into. On a high-density display (macOS Retina, Wayland with scaling)
/// a window has more pixels than points; on Windows and X11 they are equal and the OS reports
/// the scale as display_scale instead (docs/README-highdpi.md in SDL).
struct WindowMetrics {
  Size points{};
  Size pixels{};
  /// How much bigger the OS wants content drawn (e.g. 2.0 at 200%), for UI that wants to stay
  /// readable. The virtual canvas does not need it: it scales with the pixel size.
  float display_scale = 1;

  bool operator==(const WindowMetrics&) const = default;
};

/// Pixels per point (horizontal). 1 for an empty window.
[[nodiscard]] float pixel_density(const WindowMetrics& metrics);

/// Converts a point in window points to window pixels.
[[nodiscard]] PointF points_to_pixels(const WindowMetrics& metrics, PointF point);

/// Where the virtual canvas lands in the window.
struct Viewport {
  /// The canvas rectangle in window pixels. In kFill it can extend past the window edges.
  RectF rect{};
  /// Window pixels per virtual pixel. 0 when the window or the canvas is empty.
  float scale = 0;
};

/// Places a canvas of `virtual_size` in a window of `window_pixels` (its size in pixels, not
/// points). The canvas is centred; its top-left corner sits on a whole pixel.
[[nodiscard]] Viewport compute_viewport(Size virtual_size, Size window_pixels, ScaleMode mode);

/// Maps a point on the virtual canvas to window pixels.
[[nodiscard]] PointF virtual_to_window(const Viewport& viewport, PointF virtual_point);

/// Maps a point in window pixels (e.g. the pointer) to the virtual canvas. Points in a letterbox
/// bar land outside [0, width) x [0, height) of the canvas; callers hit-test against that. An
/// empty viewport maps everything to the origin.
[[nodiscard]] PointF window_to_virtual(const Viewport& viewport, PointF window_point);

}  // namespace vnmaki
