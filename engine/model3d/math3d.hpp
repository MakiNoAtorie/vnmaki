#pragma once

// The little 3D maths a scene needs (docs/SPEC.md section 12). Matrices are row-major and take
// column vectors, as in engine/render/mesh.hpp.

#include "engine/render/mesh.hpp"

namespace vnmaki {

struct Vec3 {
  float x = 0;
  float y = 0;
  float z = 0;
};

/// The distances of the camera's near and far planes.
inline constexpr float kNearPlane = 0.1F;
inline constexpr float kFarPlane = 1000.0F;
/// The vertical field of view a script may give, in degrees.
inline constexpr float kMinFov = 1.0F;
inline constexpr float kMaxFov = 170.0F;

[[nodiscard]] Mat4 multiply(const Mat4& left, const Mat4& right);

/// Model space to world space: scale, then rotate by `degrees` (x, y, z) in the order z, x, y
/// (`rotation_y * rotation_x * rotation_z`, so y is the turn about the vertical), then move.
[[nodiscard]] Mat4 model_matrix(Vec3 position, Vec3 degrees, float scale);

/// World space to view space for a camera at `eye` looking at `target`, with y up. A camera that
/// looks straight up or down, or at itself, still gets a valid view.
[[nodiscard]] Mat4 look_at(Vec3 eye, Vec3 target);

/// View space to clip space for a vertical field of view of `fov_degrees`: depth from 0 at
/// kNearPlane to 1 at kFarPlane, y up.
[[nodiscard]] Mat4 perspective(float fov_degrees, float aspect);

}  // namespace vnmaki
