#include "engine/model3d/animation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vnmaki {

namespace {

constexpr float kTwo = 2.0F;
constexpr float kThree = 3.0F;
constexpr std::size_t kCubicParts = 3;  // in-tangent, value, out-tangent

float lerp(float a, float b, float t) { return a + ((b - a) * t); }

Vec3 lerp(Vec3 a, Vec3 b, float t) {
  return {.x = lerp(a.x, b.x, t), .y = lerp(a.y, b.y, t), .z = lerp(a.z, b.z, t)};
}

// The time inside `clip` that a layer is at: a loop wraps, other clips stop at the end.
double clip_time(const ClipLayer& layer, const ModelClip& clip) {
  if (layer.loop) {
    return clip.duration > 0 && std::isfinite(layer.time) ? std::fmod(layer.time, clip.duration)
                                                          : 0;
  }
  return std::min(layer.time, static_cast<double>(clip.duration));
}

// Where `time` falls between the keys: the key at or before it, and how far (0 to 1) it is to the
// next key. A time before the first key or after the last key is that key.
struct KeySpan {
  std::size_t key = 0;
  float fraction = 0;
  float seconds = 0;  // the time between the two keys
};

KeySpan locate_key(const std::vector<float>& times, double time) {
  if (times.size() < 2 || time <= static_cast<double>(times.front())) {
    return {};
  }
  if (time >= static_cast<double>(times.back())) {
    return {.key = times.size() - 1};
  }
  const auto after = std::ranges::upper_bound(times, static_cast<float>(time));
  const auto key = static_cast<std::size_t>(std::distance(times.begin(), after)) - 1;
  const float seconds = times.at(key + 1) - times.at(key);
  const float fraction =
      seconds > 0 ? static_cast<float>((time - static_cast<double>(times.at(key))) / seconds) : 0;
  return {.key = key, .fraction = std::clamp(fraction, 0.0F, 1.0F), .seconds = seconds};
}

// A cubic spline stores in-tangent, value and out-tangent for each key.
std::span<const float> key_part(const AnimationChannel& channel, std::size_t key,
                                std::size_t part) {
  const bool cubic = channel.interpolation == Interpolation::kCubicSpline;
  const std::size_t first =
      cubic ? ((key * kCubicParts) + part) * channel.width : key * channel.width;
  return std::span{channel.values}.subspan(first, channel.width);
}

constexpr std::size_t kInTangent = 0;
constexpr std::size_t kValue = 1;
constexpr std::size_t kOutTangent = 2;

// The value of `channel` at `time`.
std::vector<float> sample_channel(const AnimationChannel& channel, double time) {
  const KeySpan at = locate_key(channel.times, time);
  const bool cubic = channel.interpolation == Interpolation::kCubicSpline;
  const std::size_t value_part = cubic ? kValue : 0;
  const std::span<const float> from = key_part(channel, at.key, value_part);
  std::vector<float> result(from.begin(), from.end());
  if (at.fraction <= 0 || channel.interpolation == Interpolation::kStep) {
    return result;
  }
  const std::span<const float> to = key_part(channel, at.key + 1, value_part);
  const float t = at.fraction;
  if (cubic) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    const std::span<const float> out_tangent = key_part(channel, at.key, kOutTangent);
    const std::span<const float> in_tangent = key_part(channel, at.key + 1, kInTangent);
    const float p0 = (kTwo * t3) - (kThree * t2) + 1;
    const float m0 = (t3 - (kTwo * t2) + t) * at.seconds;
    const float p1 = (-kTwo * t3) + (kThree * t2);
    const float m1 = (t3 - t2) * at.seconds;
    for (std::size_t i = 0; i < result.size(); ++i) {
      result.at(i) = (p0 * from[i]) + (m0 * out_tangent[i]) + (p1 * to[i]) + (m1 * in_tangent[i]);
    }
  } else if (channel.path == AnimationPath::kRotation) {
    const Quat blended =
        slerp({from[0], from[1], from[2], from[3]}, {to[0], to[1], to[2], to[3]}, t);
    std::ranges::copy(blended, result.begin());
  } else {
    for (std::size_t i = 0; i < result.size(); ++i) {
      result.at(i) = lerp(from[i], to[i], t);
    }
  }
  return result;
}

Mat4 local_matrix(const NodePose& node) {
  return trs_matrix(node.translation, node.rotation, node.scale);
}

// `base` becomes the blend from `base` (t = 0) to `over` (t = 1); both come from rest_pose.
void blend(ModelPose& base, const ModelPose& over, float t) {
  for (std::size_t i = 0; i < base.nodes.size(); ++i) {
    NodePose& a = base.nodes.at(i);
    const NodePose& b = over.nodes.at(i);
    a.translation = lerp(a.translation, b.translation, t);
    a.rotation = slerp(a.rotation, b.rotation, t);
    a.scale = lerp(a.scale, b.scale, t);
    std::vector<float>& weights = base.weights.at(i);
    for (std::size_t w = 0; w < weights.size() && w < over.weights.at(i).size(); ++w) {
      weights.at(w) = lerp(weights.at(w), over.weights.at(i).at(w), t);
    }
  }
}

}  // namespace

double layer_weight(const ClipLayer& layer) {
  return layer.fade > 0 ? std::clamp(layer.elapsed / layer.fade, 0.0, 1.0) : 1.0;
}

void start_clip(ClipPlayer& player, std::string clip, const PlayOptions& options) {
  ClipLayer layer{.clip = std::move(clip),
                  .loop = options.loop,
                  .speed = options.speed,
                  .time = options.seek,
                  .fade = options.fade};
  if (options.replaying) {
    layer.time = options.loop ? 0 : kEndOfClip;
    layer.fade = 0;
  }
  if (layer.fade <= 0) {
    player.layers.assign(1, std::move(layer));
    return;
  }
  if (player.layers.empty()) {
    player.layers.emplace_back();  // the pose at rest, which the clip fades in over
  }
  if (player.layers.size() >= kMaxClipLayers) {
    player.layers.erase(player.layers.begin());
  }
  player.layers.push_back(std::move(layer));
}

void advance_clips(ClipPlayer& player, const ModelData& data, double seconds) {
  for (ClipLayer& layer : player.layers) {
    layer.elapsed += seconds;
    const ModelClip* clip = layer.clip.empty() ? nullptr : find_clip(data, layer.clip);
    if (clip == nullptr) {
      layer.time = 0;
      continue;
    }
    layer.time =
        clip_time({.loop = layer.loop, .time = layer.time + (seconds * layer.speed)}, *clip);
  }
  // A layer that has faded in fully covers the older ones.
  for (std::size_t newest = player.layers.size(); newest-- > 1;) {
    if (layer_weight(player.layers.at(newest)) >= 1) {
      player.layers.erase(player.layers.begin(),
                          player.layers.begin() + static_cast<std::ptrdiff_t>(newest));
      break;
    }
  }
}

bool clips_busy(const ClipPlayer& player, const ModelData& data) {
  if (player.layers.empty()) {
    return false;
  }
  const ClipLayer& last = player.layers.back();
  const ModelClip* clip = last.clip.empty() ? nullptr : find_clip(data, last.clip);
  if (last.loop || clip == nullptr) {
    return false;
  }
  return layer_weight(last) < 1 || last.time < static_cast<double>(clip->duration);
}

void finish_clips(ClipPlayer& player) {
  if (player.layers.empty() || player.layers.back().loop) {
    return;
  }
  ClipLayer last = std::move(player.layers.back());
  last.time = kEndOfClip;
  last.fade = 0;
  player.layers.assign(1, std::move(last));
}

ClipPlayer settled(const ClipPlayer& player) {
  ClipPlayer result;
  if (player.layers.empty() || player.layers.back().clip.empty()) {
    return result;
  }
  ClipLayer last = player.layers.back();
  last.time = last.loop ? 0 : kEndOfClip;
  last.fade = 0;
  last.elapsed = 0;
  result.layers.push_back(std::move(last));
  return result;
}

const ModelClip* find_clip(const ModelData& data, std::string_view name) {
  const auto found = std::ranges::find(data.clips, name, &ModelClip::name);
  return found == data.clips.end() ? nullptr : &*found;
}

ModelPose rest_pose(const ModelData& data) {
  ModelPose pose;
  pose.nodes.reserve(data.nodes.size());
  pose.weights.reserve(data.nodes.size());
  for (const ModelNode& node : data.nodes) {
    pose.nodes.push_back(
        NodePose{.translation = node.translation, .rotation = node.rotation, .scale = node.scale});
    pose.weights.push_back(node.weights);
  }
  return pose;
}

void sample_clip(const ModelClip& clip, double time, ModelPose& pose) {
  for (const AnimationChannel& channel : clip.channels) {
    if (channel.node >= pose.nodes.size() || channel.times.empty() || channel.width == 0) {
      continue;
    }
    const std::vector<float> value = sample_channel(channel, time);
    NodePose& node = pose.nodes.at(channel.node);
    switch (channel.path) {
      case AnimationPath::kTranslation:
        node.translation = {.x = value.at(0), .y = value.at(1), .z = value.at(2)};
        break;
      case AnimationPath::kRotation:
        node.rotation = normalized(Quat{value.at(0), value.at(1), value.at(2), value.at(3)});
        break;
      case AnimationPath::kScale:
        node.scale = {.x = value.at(0), .y = value.at(1), .z = value.at(2)};
        break;
      case AnimationPath::kWeights:
        if (pose.weights.at(channel.node).size() == value.size()) {
          pose.weights.at(channel.node) = value;
        }
        break;
    }
  }
}

ModelPose evaluate_pose(const ModelData& data, const ClipPlayer& player) {
  ModelPose pose = rest_pose(data);
  bool first = true;
  for (const ClipLayer& layer : player.layers) {
    ModelPose layer_pose = rest_pose(data);
    if (const ModelClip* clip = layer.clip.empty() ? nullptr : find_clip(data, layer.clip)) {
      sample_clip(*clip, clip_time(layer, *clip), layer_pose);
    }
    if (first) {
      pose = std::move(layer_pose);
      first = false;
    } else {
      blend(pose, layer_pose, static_cast<float>(layer_weight(layer)));
    }
  }
  return pose;
}

bool set_morph_weight(const ModelData& data, ModelPose& pose, std::string_view morph,
                      float weight) {
  bool found = false;
  for (const ModelMesh& mesh : data.meshes) {
    std::vector<float>& weights = pose.weights.at(mesh.node);
    for (std::size_t i = 0; i < mesh.morphs.size() && i < weights.size(); ++i) {
      if (mesh.morphs.at(i).name == morph) {
        weights.at(i) = weight;
        found = true;
      }
    }
  }
  return found;
}

bool has_morph(const ModelData& data, std::string_view morph) {
  return std::ranges::any_of(data.meshes, [morph](const ModelMesh& mesh) {
    return std::ranges::any_of(mesh.morphs,
                               [morph](const MorphTarget& target) { return target.name == morph; });
  });
}

bool is_dynamic(const ModelData& data) {
  return std::ranges::any_of(
      data.meshes, [](const ModelMesh& mesh) { return mesh.skin >= 0 || !mesh.morphs.empty(); });
}

std::vector<std::vector<Mat4>> skin_matrices(const ModelData& data, const ModelPose& pose) {
  std::vector<Mat4> world(data.nodes.size(), identity_matrix());
  for (const std::size_t index : data.node_order) {
    const Mat4 local = local_matrix(pose.nodes.at(index));
    const int parent = data.nodes.at(index).parent;
    world.at(index) =
        parent < 0 ? local : multiply(world.at(static_cast<std::size_t>(parent)), local);
  }
  std::vector<std::vector<Mat4>> result;
  result.reserve(data.skins.size());
  for (const ModelSkin& skin : data.skins) {
    std::vector<Mat4>& joints = result.emplace_back();
    joints.reserve(skin.joints.size());
    for (std::size_t i = 0; i < skin.joints.size(); ++i) {
      joints.push_back(multiply(world.at(skin.joints.at(i)), skin.inverse_bind.at(i)));
    }
  }
  return result;
}

void pose_vertices(const ModelMesh& mesh, const ModelPose& pose,
                   std::span<const std::vector<Mat4>> skins, std::vector<MeshVertex>& out) {
  out = mesh.vertices;
  const std::vector<float>& weights = pose.weights.at(mesh.node);
  for (std::size_t target = 0; target < mesh.morphs.size() && target < weights.size(); ++target) {
    const float weight = weights.at(target);
    if (weight == 0) {
      continue;
    }
    const std::vector<std::array<float, 3>>& deltas = mesh.morphs.at(target).deltas;
    for (std::size_t i = 0; i < out.size(); ++i) {
      for (std::size_t axis = 0; axis < 3; ++axis) {
        out.at(i).position.at(axis) += weight * deltas.at(i).at(axis);
      }
    }
  }
  if (mesh.skin < 0 || static_cast<std::size_t>(mesh.skin) >= skins.size()) {
    return;
  }
  const std::vector<Mat4>& joints = skins[static_cast<std::size_t>(mesh.skin)];
  for (std::size_t i = 0; i < out.size(); ++i) {
    MeshVertex& vertex = out.at(i);
    const SkinInfluence& influence = mesh.influences.at(i);
    const Vec3 position{
        .x = vertex.position.at(0), .y = vertex.position.at(1), .z = vertex.position.at(2)};
    const Vec3 normal{.x = vertex.normal.at(0), .y = vertex.normal.at(1), .z = vertex.normal.at(2)};
    Vec3 moved{};
    Vec3 turned{};
    for (std::size_t k = 0; k < influence.joints.size(); ++k) {
      const float weight = influence.weights.at(k);
      if (weight == 0) {
        continue;
      }
      const Mat4& joint = joints.at(influence.joints.at(k));
      const Vec3 p = transform_point(joint, position);
      const Vec3 n = transform_direction(joint, normal);
      moved = {.x = moved.x + (weight * p.x),
               .y = moved.y + (weight * p.y),
               .z = moved.z + (weight * p.z)};
      turned = {.x = turned.x + (weight * n.x),
                .y = turned.y + (weight * n.y),
                .z = turned.z + (weight * n.z)};
    }
    const float length =
        std::sqrt((turned.x * turned.x) + (turned.y * turned.y) + (turned.z * turned.z));
    vertex.position = {moved.x, moved.y, moved.z};
    if (length > 0 && std::isfinite(length)) {
      vertex.normal = {turned.x / length, turned.y / length, turned.z / length};
    }
  }
}

}  // namespace vnmaki
