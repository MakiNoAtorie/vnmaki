#include "engine/model3d/math3d.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace vnmaki {

namespace {

constexpr std::size_t kDim = 4;
constexpr float kDegreesPerHalfTurn = 180.0F;
constexpr float kHalf = 0.5F;
// A look direction closer to the vertical than this uses another up vector.
constexpr float kVerticalLimit = 0.999F;
constexpr float kTiny = 1e-6F;
constexpr float kTwo = 2.0F;
constexpr float kFour = 4.0F;
// Rotations whose cosine is above this are so close that a straight blend is as good as slerp.
constexpr float kNlerpLimit = 0.9995F;

float radians(float degrees) { return degrees * std::numbers::pi_v<float> / kDegreesPerHalfTurn; }

Vec3 minus(Vec3 a, Vec3 b) { return {.x = a.x - b.x, .y = a.y - b.y, .z = a.z - b.z}; }
float dot(Vec3 a, Vec3 b) { return (a.x * b.x) + (a.y * b.y) + (a.z * b.z); }
Vec3 cross(Vec3 a, Vec3 b) {
  return {.x = (a.y * b.z) - (a.z * b.y),
          .y = (a.z * b.x) - (a.x * b.z),
          .z = (a.x * b.y) - (a.y * b.x)};
}
Vec3 normalized(Vec3 v, Vec3 fallback) {
  const float length = std::sqrt(dot(v, v));
  if (!(length > kTiny)) {
    return fallback;
  }
  return {.x = v.x / length, .y = v.y / length, .z = v.z / length};
}

float& cell(Mat4& m, std::size_t row, std::size_t column) { return m.at((row * kDim) + column); }

float cell(const Mat4& m, std::size_t row, std::size_t column) {
  return m.at((row * kDim) + column);
}

Mat4 identity() {
  Mat4 m{};
  for (std::size_t i = 0; i < kDim; ++i) {
    cell(m, i, i) = 1;
  }
  return m;
}

Mat4 rotation_x(float degrees) {
  Mat4 m = identity();
  const float c = std::cos(radians(degrees));
  const float s = std::sin(radians(degrees));
  cell(m, 1, 1) = c;
  cell(m, 1, 2) = -s;
  cell(m, 2, 1) = s;
  cell(m, 2, 2) = c;
  return m;
}

Mat4 rotation_y(float degrees) {
  Mat4 m = identity();
  const float c = std::cos(radians(degrees));
  const float s = std::sin(radians(degrees));
  cell(m, 0, 0) = c;
  cell(m, 0, 2) = s;
  cell(m, 2, 0) = -s;
  cell(m, 2, 2) = c;
  return m;
}

Mat4 rotation_z(float degrees) {
  Mat4 m = identity();
  const float c = std::cos(radians(degrees));
  const float s = std::sin(radians(degrees));
  cell(m, 0, 0) = c;
  cell(m, 0, 1) = -s;
  cell(m, 1, 0) = s;
  cell(m, 1, 1) = c;
  return m;
}

}  // namespace

Mat4 identity_matrix() { return identity(); }

Mat4 trs_matrix(Vec3 translation, Quat rotation, Vec3 scale) {
  const Quat q = normalized(rotation);
  const float x = q.at(0);
  const float y = q.at(1);
  const float z = q.at(2);
  const float w = q.at(3);
  Mat4 m = identity();
  cell(m, 0, 0) = 1 - (kTwo * ((y * y) + (z * z)));
  cell(m, 0, 1) = kTwo * ((x * y) - (z * w));
  cell(m, 0, 2) = kTwo * ((x * z) + (y * w));
  cell(m, 1, 0) = kTwo * ((x * y) + (z * w));
  cell(m, 1, 1) = 1 - (kTwo * ((x * x) + (z * z)));
  cell(m, 1, 2) = kTwo * ((y * z) - (x * w));
  cell(m, 2, 0) = kTwo * ((x * z) - (y * w));
  cell(m, 2, 1) = kTwo * ((y * z) + (x * w));
  cell(m, 2, 2) = 1 - (kTwo * ((x * x) + (y * y)));
  const std::array<float, 3> factors{scale.x, scale.y, scale.z};
  for (std::size_t column = 0; column < 3; ++column) {
    for (std::size_t row = 0; row < 3; ++row) {
      cell(m, row, column) *= factors.at(column);
    }
  }
  cell(m, 0, 3) = translation.x;
  cell(m, 1, 3) = translation.y;
  cell(m, 2, 3) = translation.z;
  return m;
}

void decompose(const Mat4& m, Vec3& translation, Quat& rotation, Vec3& scale) {
  translation = {.x = cell(m, 0, 3), .y = cell(m, 1, 3), .z = cell(m, 2, 3)};
  std::array<float, 3> lengths{};
  for (std::size_t column = 0; column < 3; ++column) {
    const Vec3 axis{.x = cell(m, 0, column), .y = cell(m, 1, column), .z = cell(m, 2, column)};
    lengths.at(column) = std::sqrt(dot(axis, axis));
  }
  scale = {.x = lengths.at(0), .y = lengths.at(1), .z = lengths.at(2)};
  // The rotation matrix: the columns divided by their lengths.
  const auto r = [&](std::size_t row, std::size_t column) {
    return lengths.at(column) > kTiny ? cell(m, row, column) / lengths.at(column)
                                      : (row == column ? 1.0F : 0.0F);
  };
  const float trace = r(0, 0) + r(1, 1) + r(2, 2);
  if (trace > 0) {
    const float s = std::sqrt(trace + 1) * kTwo;
    rotation = {(r(2, 1) - r(1, 2)) / s, (r(0, 2) - r(2, 0)) / s, (r(1, 0) - r(0, 1)) / s,
                s / kFour};
  } else if (r(0, 0) > r(1, 1) && r(0, 0) > r(2, 2)) {
    const float s = std::sqrt(1 + r(0, 0) - r(1, 1) - r(2, 2)) * kTwo;
    rotation = {s / kFour, (r(0, 1) + r(1, 0)) / s, (r(0, 2) + r(2, 0)) / s,
                (r(2, 1) - r(1, 2)) / s};
  } else if (r(1, 1) > r(2, 2)) {
    const float s = std::sqrt(1 + r(1, 1) - r(0, 0) - r(2, 2)) * kTwo;
    rotation = {(r(0, 1) + r(1, 0)) / s, s / kFour, (r(1, 2) + r(2, 1)) / s,
                (r(0, 2) - r(2, 0)) / s};
  } else {
    const float s = std::sqrt(1 + r(2, 2) - r(0, 0) - r(1, 1)) * kTwo;
    rotation = {(r(0, 2) + r(2, 0)) / s, (r(1, 2) + r(2, 1)) / s, s / kFour,
                (r(1, 0) - r(0, 1)) / s};
  }
  rotation = normalized(rotation);
}

Quat normalized(const Quat& v) {
  const float squared =
      (v.at(0) * v.at(0)) + (v.at(1) * v.at(1)) + (v.at(2) * v.at(2)) + (v.at(3) * v.at(3));
  if (!(squared > kTiny) || !std::isfinite(squared)) {
    return kIdentityQuat;
  }
  const float length = std::sqrt(squared);
  return {v.at(0) / length, v.at(1) / length, v.at(2) / length, v.at(3) / length};
}

Quat slerp(const Quat& a, const Quat& b, float t) {
  float cosine =
      (a.at(0) * b.at(0)) + (a.at(1) * b.at(1)) + (a.at(2) * b.at(2)) + (a.at(3) * b.at(3));
  const float sign = cosine < 0 ? -1.0F : 1.0F;
  cosine *= sign;
  float from = 1 - t;
  float to = t;
  if (cosine < kNlerpLimit) {  // far enough apart for the sine formula
    const float angle = std::acos(cosine);
    const float sine = std::sin(angle);
    from = std::sin((1 - t) * angle) / sine;
    to = std::sin(t * angle) / sine;
  }
  return normalized(
      {(from * a.at(0)) + (to * sign * b.at(0)), (from * a.at(1)) + (to * sign * b.at(1)),
       (from * a.at(2)) + (to * sign * b.at(2)), (from * a.at(3)) + (to * sign * b.at(3))});
}

Vec3 transform_point(const Mat4& m, Vec3 v) {
  return {
      .x = (cell(m, 0, 0) * v.x) + (cell(m, 0, 1) * v.y) + (cell(m, 0, 2) * v.z) + cell(m, 0, 3),
      .y = (cell(m, 1, 0) * v.x) + (cell(m, 1, 1) * v.y) + (cell(m, 1, 2) * v.z) + cell(m, 1, 3),
      .z = (cell(m, 2, 0) * v.x) + (cell(m, 2, 1) * v.y) + (cell(m, 2, 2) * v.z) + cell(m, 2, 3)};
}

Vec3 transform_direction(const Mat4& m, Vec3 v) {
  return {.x = (cell(m, 0, 0) * v.x) + (cell(m, 0, 1) * v.y) + (cell(m, 0, 2) * v.z),
          .y = (cell(m, 1, 0) * v.x) + (cell(m, 1, 1) * v.y) + (cell(m, 1, 2) * v.z),
          .z = (cell(m, 2, 0) * v.x) + (cell(m, 2, 1) * v.y) + (cell(m, 2, 2) * v.z)};
}

Mat4 multiply(const Mat4& left, const Mat4& right) {
  Mat4 result{};
  for (std::size_t row = 0; row < kDim; ++row) {
    for (std::size_t column = 0; column < kDim; ++column) {
      float sum = 0;
      for (std::size_t k = 0; k < kDim; ++k) {
        sum += cell(left, row, k) * cell(right, k, column);
      }
      cell(result, row, column) = sum;
    }
  }
  return result;
}

Mat4 model_matrix(Vec3 position, Vec3 degrees, float scale) {
  Mat4 m = multiply(rotation_y(degrees.y), multiply(rotation_x(degrees.x), rotation_z(degrees.z)));
  for (std::size_t row = 0; row < 3; ++row) {
    for (std::size_t column = 0; column < 3; ++column) {
      cell(m, row, column) *= scale;
    }
  }
  cell(m, 0, 3) = position.x;
  cell(m, 1, 3) = position.y;
  cell(m, 2, 3) = position.z;
  return m;
}

Mat4 look_at(Vec3 eye, Vec3 target) {
  const Vec3 forward = normalized(minus(target, eye), {.x = 0, .y = 0, .z = -1});
  Vec3 up{.x = 0, .y = 1, .z = 0};
  if (std::abs(dot(forward, up)) > kVerticalLimit) {
    up = {.x = 0, .y = 0, .z = forward.y > 0 ? 1.0F : -1.0F};
  }
  const Vec3 right = normalized(cross(forward, up), {.x = 1, .y = 0, .z = 0});
  const Vec3 true_up = cross(right, forward);
  Mat4 m = identity();
  const std::array<Vec3, 3> axes{
      right, true_up, {.x = -forward.x, .y = -forward.y, .z = -forward.z}};
  for (std::size_t row = 0; row < 3; ++row) {
    const Vec3 axis = axes.at(row);
    cell(m, row, 0) = axis.x;
    cell(m, row, 1) = axis.y;
    cell(m, row, 2) = axis.z;
    cell(m, row, 3) = -dot(axis, eye);
  }
  return m;
}

Mat4 perspective(float fov_degrees, float aspect) {
  const float focal = 1.0F / std::tan(radians(fov_degrees) * kHalf);
  const float depth = kFarPlane / (kNearPlane - kFarPlane);
  Mat4 m{};
  cell(m, 0, 0) = focal / aspect;
  cell(m, 1, 1) = focal;
  cell(m, 2, 2) = depth;
  cell(m, 2, 3) = kNearPlane * depth;
  cell(m, 3, 2) = -1;
  return m;
}

}  // namespace vnmaki
