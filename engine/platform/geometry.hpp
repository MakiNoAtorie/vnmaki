#pragma once

namespace vnmaki {

/// A size in whole pixels (or points, where documented).
struct Size {
  int width = 0;
  int height = 0;

  bool operator==(const Size&) const = default;
};

/// A point. Which space it lives in (window pixels, virtual pixels) is up to the caller.
struct PointF {
  float x = 0;
  float y = 0;

  bool operator==(const PointF&) const = default;
};

/// An axis-aligned rectangle: top-left corner plus size.
struct RectF {
  float x = 0;
  float y = 0;
  float width = 0;
  float height = 0;

  bool operator==(const RectF&) const = default;
};

}  // namespace vnmaki
