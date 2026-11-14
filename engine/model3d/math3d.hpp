#pragma once

// The little 3D maths a scene needs (docs/SPEC.md section 12). Matrices are row-major and take
// column vectors, as in engine/render/mesh.hpp.

#include <array>

#include "engine/render/mesh.hpp"

namespace vnmaki {

struct Vec3 {
  float x = 0;
  float y = 0;
  float z = 0;
};

/// A rotation as a quaternion (x, y, z, w), as glTF has it. The identity is (0, 0, 0, 1).
using Quat = std::array<float, 4>;
inline constexpr Quat kIdentityQuat{0, 0, 0, 1};

/// The distances of the camera's near and far planes.
inline constexpr float kNearPlane = 0.1F;
inline constexpr float kFarPlane = 1000.0F;
/// The vertical field of view a script may give, in degrees.
inline constexpr float kMinFov = 1.0F;
inline constexpr float kMaxFov = 170.0F;

/// The identity matrix.
[[nodiscard]] Mat4 identity_matrix();

/// Translation * rotation * scale for a node of a glTF file. `rotation` is normalized first (a
/// zero quaternion is the identity).
[[nodiscard]] Mat4 trs_matrix(Vec3 translation, Quat rotation, Vec3 scale);

/// Splits an affine matrix into translation, rotation and scale (shear is lost). A scale of 0
/// along an axis gives that axis a scale of 0 and no rotation of its own.
void decompose(const Mat4& m, Vec3& translation, Quat& rotation, Vec3& scale);

/// The rotation halfway `t` (0 to 1) from `a` to `b` by the short way, normalized.
[[nodiscard]] Quat slerp(const Quat& a, const Quat& b, float t);

/// `v` scaled to length 1, or the identity when its length is 0 or not finite.
[[nodiscard]] Quat normalized(const Quat& v);

/// A point (w = 1) or a direction (w = 0) times the matrix.
[[nodiscard]] Vec3 transform_point(const Mat4& m, Vec3 v);
[[nodiscard]] Vec3 transform_direction(const Mat4& m, Vec3 v);

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
