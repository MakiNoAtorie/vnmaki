// Virtual resolution: where the virtual canvas lands in the window, and how
// points map between the two. No window needed: these are pure functions.
// Expected values are worked out by hand from the window and virtual sizes.

#include "engine/platform/viewport.hpp"

#include <doctest/doctest.h>

#include "tests/platform/printers.hpp"

using vnmaki::PointF;
using vnmaki::RectF;
using vnmaki::ScaleMode;
using vnmaki::Size;

namespace {

// Non-integer scales from the worked examples below.
constexpr double kOneAndAHalf = 1.5;
constexpr double kHalf = 0.5;
constexpr float kHalfPixel = 0.5F;

}  // namespace

TEST_CASE("viewport: fit letterboxes with bars above and below in a taller window") {
  // 1280x720 into 1920x1200: scale min(1.5, 1.667) = 1.5 -> 1920x1080, 60 px bars.
  const auto viewport = vnmaki::compute_viewport({1280, 720}, {1920, 1200}, ScaleMode::kFit);

  CHECK(viewport.scale == doctest::Approx(kOneAndAHalf));
  CHECK(viewport.rect == RectF{0, 60, 1920, 1080});
}

TEST_CASE("viewport: fit pillarboxes with bars left and right in a wider window") {
  // 1280x720 into 2560x1080: scale min(2, 1.5) = 1.5 -> 1920x1080, 320 px bars.
  const auto viewport = vnmaki::compute_viewport({1280, 720}, {2560, 1080}, ScaleMode::kFit);

  CHECK(viewport.scale == doctest::Approx(kOneAndAHalf));
  CHECK(viewport.rect == RectF{320, 0, 1920, 1080});
}

TEST_CASE("viewport: fill covers the whole window and crops the overflowing canvas edges") {
  // 100x100 into 200x300: scale max(2, 3) = 3 -> 300x300, 50 px cropped on each side.
  const auto viewport = vnmaki::compute_viewport({100, 100}, {200, 300}, ScaleMode::kFill);

  CHECK(viewport.scale == doctest::Approx(3));
  CHECK(viewport.rect == RectF{-50, 0, 300, 300});
}

TEST_CASE("viewport: integer scale uses the largest whole-number scale that fits") {
  // 640x360 into 1600x1000: fit would be 2.5; integer is 2 -> 1280x720 at (160, 140).
  const auto viewport = vnmaki::compute_viewport({640, 360}, {1600, 1000}, ScaleMode::kInteger);

  CHECK(viewport.scale == doctest::Approx(2));
  CHECK(viewport.rect == RectF{160, 140, 1280, 720});
}

TEST_CASE("viewport: integer scale falls back to fit when the window is smaller than the canvas") {
  // 640x360 into 320x200: fit = min(0.5, 0.556) = 0.5 -> 320x180 at y = 10.
  const auto viewport = vnmaki::compute_viewport({640, 360}, {320, 200}, ScaleMode::kInteger);

  CHECK(viewport.scale == doctest::Approx(kHalf));
  CHECK(viewport.rect == RectF{0, 10, 320, 180});
}

TEST_CASE("viewport: the canvas origin snaps to a whole window pixel") {
  // 100x100 into 201x101 (fit): scale 1.01 -> 101x101, spare 100 px -> x = 50, y = 0.
  // 100x100 into 303x200 (integer): scale 2 -> 200x200, spare 103 px -> x = floor(51.5) = 51.
  CHECK(vnmaki::compute_viewport({100, 100}, {201, 101}, ScaleMode::kFit).rect.x == 50);
  CHECK(vnmaki::compute_viewport({100, 100}, {303, 200}, ScaleMode::kInteger).rect.x == 51);
}

TEST_CASE("viewport: an empty window (minimized) gives an empty viewport, not a division by zero") {
  for (const ScaleMode mode : {ScaleMode::kFit, ScaleMode::kFill, ScaleMode::kInteger}) {
    const auto viewport = vnmaki::compute_viewport({1280, 720}, {0, 0}, mode);
    CHECK(viewport.scale == 0);
    CHECK(viewport.rect == RectF{});
  }
  CHECK(vnmaki::compute_viewport({0, 720}, {1280, 720}, ScaleMode::kFit).scale == 0);
}

TEST_CASE("viewport: maps virtual points to window pixels and back") {
  // 640x360 into 1600x1000 (integer): scale 2, canvas at (160, 140).
  const auto viewport = vnmaki::compute_viewport({640, 360}, {1600, 1000}, ScaleMode::kInteger);

  CHECK(vnmaki::virtual_to_window(viewport, {0, 0}) == PointF{160, 140});
  CHECK(vnmaki::virtual_to_window(viewport, {640, 360}) == PointF{1440, 860});
  CHECK(vnmaki::window_to_virtual(viewport, {800, 500}) == PointF{320, 180});
  CHECK(vnmaki::window_to_virtual(viewport, {161, 141}) == PointF{kHalfPixel, kHalfPixel});
}

TEST_CASE("viewport: a window point in a letterbox bar maps outside the canvas") {
  // 1280x720 into 1920x1200 (fit): scale 1.5, canvas at y = 60. The top bar is y < 60.
  const auto viewport = vnmaki::compute_viewport({1280, 720}, {1920, 1200}, ScaleMode::kFit);

  CHECK(vnmaki::window_to_virtual(viewport, {30, 30}) == PointF{20, -20});
  CHECK(vnmaki::window_to_virtual(viewport, {1920, 1170}) == PointF{1280, 740});
}

TEST_CASE("viewport: in fill mode window corners map inside the cropped canvas") {
  // 100x100 into 200x300 (fill): scale 3, canvas at (-50, 0): 50/3 virtual px cropped per side.
  const auto viewport = vnmaki::compute_viewport({100, 100}, {200, 300}, ScaleMode::kFill);

  constexpr double kCroppedVirtualPixels = 50.0 / 3;
  const PointF top_left = vnmaki::window_to_virtual(viewport, {0, 0});
  CHECK(top_left.x == doctest::Approx(kCroppedVirtualPixels));
  CHECK(top_left.y == 0);
}

TEST_CASE("dpi: a high-density window has more pixels than points") {
  // A 1280x720 pt window on a 2x (Retina-style) display is 2560x1440 px.
  const vnmaki::WindowMetrics retina{.points = {1280, 720}, .pixels = {2560, 1440}};
  const vnmaki::WindowMetrics standard{.points = {1280, 720}, .pixels = {1280, 720}};

  CHECK(vnmaki::pixel_density(retina) == doctest::Approx(2));
  CHECK(vnmaki::pixel_density(standard) == doctest::Approx(1));
  CHECK(vnmaki::points_to_pixels(retina, {100, 50}) == PointF{200, 100});
  CHECK(vnmaki::points_to_pixels(standard, {100, 50}) == PointF{100, 50});
}

TEST_CASE("dpi: the viewport is computed from pixels, so a 2x window renders the canvas at 2x") {
  const vnmaki::WindowMetrics retina{.points = {1280, 720}, .pixels = {2560, 1440}};
  const auto viewport = vnmaki::compute_viewport({1280, 720}, retina.pixels, ScaleMode::kFit);

  CHECK(viewport.scale == doctest::Approx(2));
  // The pointer at the window centre in points is the canvas centre.
  const PointF pointer = vnmaki::points_to_pixels(retina, {640, 360});
  CHECK(vnmaki::window_to_virtual(viewport, pointer) == PointF{640, 360});
}

TEST_CASE("dpi: an empty (minimized) window has density 1 and keeps points as they are") {
  const vnmaki::WindowMetrics minimized{.points = {0, 0}, .pixels = {0, 0}};

  CHECK(vnmaki::pixel_density(minimized) == doctest::Approx(1));
  CHECK(vnmaki::points_to_pixels(minimized, {3, 4}) == PointF{3, 4});
}

TEST_CASE("viewport: an empty viewport maps every window point to the virtual origin") {
  const auto viewport = vnmaki::compute_viewport({1280, 720}, {0, 0}, ScaleMode::kFit);

  CHECK(vnmaki::window_to_virtual(viewport, {10, 20}) == PointF{0, 0});
}
