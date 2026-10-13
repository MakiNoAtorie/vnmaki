#pragma once

// Lets doctest print platform value types in failed assertions.

#include <string>

#include <doctest/doctest.h>

#include "engine/platform/geometry.hpp"

namespace doctest {

template <>
struct StringMaker<vnmaki::Size> {
  static String convert(const vnmaki::Size& size) {
    return (std::to_string(size.width) + "x" + std::to_string(size.height)).c_str();
  }
};

template <>
struct StringMaker<vnmaki::PointF> {
  static String convert(const vnmaki::PointF& point) {
    return ("(" + std::to_string(point.x) + ", " + std::to_string(point.y) + ")").c_str();
  }
};

template <>
struct StringMaker<vnmaki::RectF> {
  static String convert(const vnmaki::RectF& rect) {
    return ("{" + std::to_string(rect.x) + ", " + std::to_string(rect.y) + ", " +
            std::to_string(rect.width) + "x" + std::to_string(rect.height) + "}")
        .c_str();
  }
};

}  // namespace doctest
