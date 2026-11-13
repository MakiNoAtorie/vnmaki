#pragma once

// Playing the clips of a glTF model (docs/SPEC.md section 12), all on the CPU: a ClipPlayer says
// which clips play and how far they are, evaluate_pose() turns that into the pose of every node
// and the weights of every morph target, skin_matrices() and pose_vertices() move the vertices.
// Nothing here has a clock or a GPU: the caller gives the elapsed seconds to advance(), and the
// scene uploads the vertices.

#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/model3d/math3d.hpp"
#include "engine/model3d/model.hpp"

namespace vnmaki {

/// The time of a clip that does not loop and is at its end, whatever its length.
inline constexpr double kEndOfClip = std::numeric_limits<double>::infinity();
/// Crossfades that run at once: a `play` past this drops the oldest clip.
inline constexpr std::size_t kMaxClipLayers = 8;
/// The most a script may give for `fade`, `speed` and `seek`.
inline constexpr double kMaxFadeSeconds = 3600;
inline constexpr double kMaxClipSpeed = 100;
inline constexpr double kMaxSeekSeconds = 86400;

/// One clip that plays, and the crossfade into it.
struct ClipLayer {
  /// The clip's name, or empty for the pose at rest (what a first crossfade starts from).
  std::string clip{};
  bool loop = false;
  double speed = 1;
  /// Seconds into the clip. kEndOfClip puts a clip that does not loop at its end.
  double time = 0;
  /// Seconds the layer takes to fade in over the older ones, and seconds since it started.
  double fade = 0;
  double elapsed = 0;
};

/// The clips of one model, oldest first. The pose is the oldest layer, then each newer layer
/// blended over it by its weight (`layer_weight`). A layer that has faded in fully drops the older
/// ones.
struct ClipPlayer {
  std::vector<ClipLayer> layers{};
};

struct PlayOptions {
  bool loop = false;
  /// Seconds of crossfade from what plays now. 0 is a cut.
  double fade = 0;
  double speed = 1;
  /// Seconds into the clip to start at.
  double seek = 0;
  /// A replay (a load, a rollback): a clip that does not loop goes to its end, a loop starts at
  /// time 0, and there is no fade.
  bool replaying = false;
};

/// 1 for a layer without a fade, else the fraction of the fade that has passed (linear).
[[nodiscard]] double layer_weight(const ClipLayer& layer);

/// Starts `clip` (a crossfade when `options.fade` is above 0).
void start_clip(ClipPlayer& player, std::string clip, const PlayOptions& options);

/// Moves every layer on by `seconds` (times its speed). A clip that does not loop stops at its
/// end; a loop wraps. A name that is not a clip of `data` stays at time 0.
void advance_clips(ClipPlayer& player, const ModelData& data, double seconds);

/// True while the newest clip does not loop and has not reached its end or finished fading in.
[[nodiscard]] bool clips_busy(const ClipPlayer& player, const ModelData& data);

/// Puts the newest clip at its end (and drops the others) if it does not loop. A loop keeps going.
void finish_clips(ClipPlayer& player);

/// What a snapshot keeps: the newest clip with its loop flag and speed, a clip that does not loop
/// at its end, a loop at time 0, no fade. Empty when nothing plays.
[[nodiscard]] ClipPlayer settled(const ClipPlayer& player);

[[nodiscard]] const ModelClip* find_clip(const ModelData& data, std::string_view name);

/// The local transform of each node, and the weights of the morph targets of each node (empty for
/// a node without morph targets).
struct NodePose {
  Vec3 translation{};
  Quat rotation = kIdentityQuat;
  Vec3 scale{.x = 1, .y = 1, .z = 1};
};
struct ModelPose {
  std::vector<NodePose> nodes{};
  std::vector<std::vector<float>> weights{};
};

/// The pose the file has without any clip.
[[nodiscard]] ModelPose rest_pose(const ModelData& data);

/// Writes the value of each channel of `clip` at `time` into `pose` (other nodes are unchanged).
/// A time outside the clip uses the first or last key. Interpolation: linear (rotations by slerp),
/// step, and cubic spline (Hermite, with the tangents of the file). `time` is not wrapped.
void sample_clip(const ModelClip& clip, double time, ModelPose& pose);

/// The pose of the player's layers blended by their weights.
[[nodiscard]] ModelPose evaluate_pose(const ModelData& data, const ClipPlayer& player);

/// Sets the weight of the morph target `morph` of every mesh that has one. Returns false when no
/// mesh has it.
bool set_morph_weight(const ModelData& data, ModelPose& pose, std::string_view morph, float weight);
/// True if a mesh of `data` has a morph target of this name.
[[nodiscard]] bool has_morph(const ModelData& data, std::string_view morph);
/// True for a model that has a skin or a morph target: its vertices change with the pose.
[[nodiscard]] bool is_dynamic(const ModelData& data);

/// For each skin of `data`, the matrix of each joint (world transform of the joint node times its
/// inverse bind matrix): what moves a vertex of the file from the bind pose to `pose`.
[[nodiscard]] std::vector<std::vector<Mat4>> skin_matrices(const ModelData& data,
                                                           const ModelPose& pose);

/// The vertices of `mesh` in `pose`: the morph targets added by weight, then the skin applied.
/// `skins` is what skin_matrices() returned. The normals are moved by the joints and normalized
/// (morph targets do not change them).
void pose_vertices(const ModelMesh& mesh, const ModelPose& pose,
                   std::span<const std::vector<Mat4>> skins, std::vector<MeshVertex>& out);

}  // namespace vnmaki
