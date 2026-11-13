#include "engine/model3d/math3d.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace vnmaki {

namespace {

constexpr int kDim = 4;
constexpr float kDegreesPerHalfTurn = 180.0F;
constexpr float kHalf = 0.5F;
// A look direction closer to the vertical than this uses another up vector.
constexpr float kVerticalLimit = 0.999F;
constexpr float kTiny = 1e-6F;

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

float& cell(Mat4& m, int row, int column) {
  return m.at(static_cast<std::size_t>((row * kDim) + column));
}

float cell(const Mat4& m, int row, int column) {
  return m.at(static_cast<std::size_t>((row * kDim) + column));
}

Mat4 identity() {
  Mat4 m{};
  for (int i = 0; i < kDim; ++i) {
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

Mat4 multiply(const Mat4& left, const Mat4& right) {
  Mat4 result{};
  for (int row = 0; row < kDim; ++row) {
    for (int column = 0; column < kDim; ++column) {
      float sum = 0;
      for (int k = 0; k < kDim; ++k) {
        sum += cell(left, row, k) * cell(right, k, column);
      }
      cell(result, row, column) = sum;
    }
  }
  return result;
}

Mat4 model_matrix(Vec3 position, Vec3 degrees, float scale) {
  Mat4 m = multiply(rotation_y(degrees.y), multiply(rotation_x(degrees.x), rotation_z(degrees.z)));
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
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
  for (int row = 0; row < 3; ++row) {
    const Vec3 axis = axes.at(static_cast<std::size_t>(row));
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
