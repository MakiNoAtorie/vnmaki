// 3D animation (docs/SPEC.md section 12, ticket 47): the clips of the generated skinned bar and
// morph quad (tools/generators/model_fixtures.py). The clock is the seconds that a test gives to
// advance_clips() or Scene::update(), so nothing here depends on real time. The GPU case is in
// tests/render/model3d_render_test.cpp.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/anim/wait_presenter.hpp"
#include "engine/audio/voice_level.hpp"
#include "engine/model3d/animation.hpp"
#include "engine/model3d/math3d.hpp"
#include "engine/model3d/model.hpp"
#include "engine/model3d/scene3d.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"
#include "fuzz/model_properties.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::ClipPlayer;
using vnmaki::ModelData;
using vnmaki::PlayOptions;
using vnmaki::Scene;
using vnmaki::Scene3dDisplayable;
using vnmaki::Vec3;

constexpr vnmaki::Size kVirtualSize{.width = 64, .height = 64};
constexpr float kTolerance = 1e-4F;
constexpr double kQuarterSecond = 0.25;
constexpr double kHalfSecond = 0.5;
constexpr double kSecond = 1.0;
constexpr float kRoot2Half = 0.70710678F;  // sin 45 degrees = cos 45 degrees
constexpr std::size_t kBarVertices = 12;
constexpr std::size_t kTopRingVertex = 8;     // (-0.25, 2, -0.25), moved by the tip joint only
constexpr std::size_t kMiddleRingVertex = 4;  // (-0.25, 1, -0.25), half root and half tip
constexpr float kBarHalf = 0.25F;
// "slide" is a cubic spline from 0 to 1 along x with an out-tangent of 4 at the first key.
constexpr float kSlideQuarter = 0.71875F;
constexpr float kSlideHalf = 1.0F;
constexpr float kOpenWeight = 0.8F;
constexpr double kVoiceLevel = 0.6;
constexpr std::size_t kManyPlays = 12;
constexpr int kMutations = 300;
constexpr int kEdits = 4;
constexpr std::uint32_t kSeed = 47;

vnmaki::Vfs fixture_vfs() {
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_FIXTURES_DIR} /
                                                 "model3d" / "game")));
  return vfs;
}

ModelData load(std::string_view path) {
  const vnmaki::Vfs vfs = fixture_vfs();
  return vnmaki::load_model(vfs, path);
}

// The pose that `clip` has `time` seconds in (a time past the end is the end).
vnmaki::ModelPose pose_at(const ModelData& data, std::string_view clip, double time) {
  ClipPlayer player;
  vnmaki::start_clip(player, std::string{clip}, PlayOptions{.seek = time});
  return vnmaki::evaluate_pose(data, player);
}

void check_point(Vec3 actual, Vec3 expected) {
  CHECK(actual.x == doctest::Approx(expected.x).epsilon(kTolerance));
  CHECK(actual.y == doctest::Approx(expected.y).epsilon(kTolerance));
  CHECK(actual.z == doctest::Approx(expected.z).epsilon(kTolerance));
}

// Where the tip joint (the second one) puts the point of the bar that is `height` above the origin
// when the pose is `pose`.
Vec3 tip_point(const ModelData& data, const vnmaki::ModelPose& pose, float height) {
  const auto skins = vnmaki::skin_matrices(data, pose);
  return vnmaki::transform_point(skins.at(0).at(1), {.x = 0, .y = height, .z = 0});
}

// --- The loader ------------------------------------------------------------------------------

TEST_CASE("anim: the skinned bar loads with its nodes, skin, weights and clips") {
  const ModelData data = load("models/skinned.gltf");
  CHECK(vnmaki::fuzz::model_is_consistent(data));
  REQUIRE(data.nodes.size() == 3);
  CHECK(data.nodes.at(1).parent == 0);
  CHECK(data.nodes.at(1).translation.y == 1);
  REQUIRE(data.skins.size() == 1);
  CHECK(data.skins.at(0).joints == std::vector<std::size_t>{0, 1});
  REQUIRE(data.meshes.size() == 1);
  const vnmaki::ModelMesh& bar = data.meshes.front();
  CHECK(bar.vertices.size() == kBarVertices);
  CHECK(bar.skin == 0);
  CHECK(bar.node == 2);
  // A skinned mesh keeps the vertices of the file: the node transform is not applied.
  CHECK(bar.vertices.at(kTopRingVertex).position == std::array<float, 3>{-kBarHalf, 2, -kBarHalf});
  CHECK(bar.influences.at(kMiddleRingVertex).weights.at(0) == doctest::Approx(0.5F));
  CHECK(bar.influences.at(kMiddleRingVertex).joints.at(1) == 1);
  REQUIRE(data.clips.size() == 4);
  CHECK(data.clips.at(0).name == "bend");
  CHECK(data.clips.at(0).duration == 1);
  CHECK(vnmaki::find_clip(data, "snap")->duration == 2);
  CHECK(vnmaki::find_clip(data, "nope") == nullptr);
  CHECK(vnmaki::is_dynamic(data));
}

TEST_CASE("anim: the morph quad loads with named targets and default weights") {
  const ModelData data = load("models/morph.gltf");
  CHECK(vnmaki::fuzz::model_is_consistent(data));
  REQUIRE(data.meshes.size() == 1);
  const vnmaki::ModelMesh& quad = data.meshes.front();
  REQUIRE(quad.morphs.size() == 2);
  CHECK(quad.morphs.at(0).name == "mouth_open");
  CHECK(quad.morphs.at(1).name == "smile");
  CHECK(quad.morphs.at(0).deltas.at(2) == std::array<float, 3>{0, 1, 0});
  CHECK(data.nodes.at(0).weights == std::vector<float>{0, 0});
  CHECK(vnmaki::has_morph(data, "smile"));
  CHECK_FALSE(vnmaki::has_morph(data, "frown"));
  CHECK(data.clips.at(0).channels.at(0).path == vnmaki::AnimationPath::kWeights);
}

TEST_CASE("anim: a bad joint index and times that go back are refused") {
  const vnmaki::Vfs vfs = fixture_vfs();
  CHECK_THROWS_AS(static_cast<void>(vnmaki::load_model(vfs, "models/bad/joint_range.gltf")), vnmaki::ModelError);
  CHECK_THROWS_AS(static_cast<void>(vnmaki::load_model(vfs, "models/bad/time_back.gltf")), vnmaki::ModelError);
}

TEST_CASE("anim: a damaged skinned model loads or fails, and its rig never points out of range") {
  const vnmaki::Vfs vfs = fixture_vfs();
  std::mt19937 random{kSeed};  // NOLINT(bugprone-random-generator-seed)
  int loaded = 0;
  for (const char* path : {"models/skinned.gltf", "models/morph.gltf"}) {
    const std::optional<std::vector<std::byte>> original = vfs.read(path);
    REQUIRE(original.has_value());
    for (int i = 0; i < kMutations; ++i) {
      std::vector<std::byte> bytes = *original;
      for (int edit = 0; edit < kEdits; ++edit) {
        bytes.at(random() % bytes.size()) = static_cast<std::byte>(random());
      }
      try {
        const ModelData data = vnmaki::parse_model(vfs, path, bytes);
        CHECK(vnmaki::fuzz::model_is_consistent(data));
        // Whatever loaded can be played to the end of every clip.
        for (const vnmaki::ModelClip& clip : data.clips) {
          const vnmaki::ModelPose pose = pose_at(data, clip.name, kSecond);
          const auto skins = vnmaki::skin_matrices(data, pose);
          std::vector<vnmaki::MeshVertex> vertices;
          for (const vnmaki::ModelMesh& mesh : data.meshes) {
            vnmaki::pose_vertices(mesh, pose, skins, vertices);
            CHECK(vertices.size() == mesh.vertices.size());
          }
        }
        ++loaded;
      } catch (const vnmaki::ModelError&) {
        // A refused file is fine.
      }
    }
  }
  CHECK(loaded > 0);
}

// --- Joint matrices --------------------------------------------------------------------------

TEST_CASE("anim: joint matrices follow a linear rotation clip") {
  const ModelData data = load("models/skinned.gltf");
  // At rest and at time 0 the joints leave the bind pose alone.
  check_point(tip_point(data, vnmaki::rest_pose(data), 2), {.x = 0, .y = 2, .z = 0});
  check_point(tip_point(data, pose_at(data, "bend", 0), 2), {.x = 0, .y = 2, .z = 0});
  // Half way the tip has turned 45 degrees about z, around the joint at height 1.
  check_point(tip_point(data, pose_at(data, "bend", kHalfSecond), 2),
              {.x = -kRoot2Half, .y = 1 + kRoot2Half, .z = 0});
  // At the end 90 degrees: the point 1 above the joint is 1 to its left. Later times are the end.
  check_point(tip_point(data, pose_at(data, "bend", kSecond), 2), {.x = -1, .y = 1, .z = 0});
  check_point(tip_point(data, pose_at(data, "bend", 100.0), 2), {.x = -1, .y = 1, .z = 0});
  // The root joint is not animated by "bend".
  const auto skins = vnmaki::skin_matrices(data, pose_at(data, "bend", kSecond));
  check_point(vnmaki::transform_point(skins.at(0).at(0), {.x = 0.3F, .y = 0.4F, .z = 0.5F}),
              {.x = 0.3F, .y = 0.4F, .z = 0.5F});
}

TEST_CASE("anim: a step clip holds each key and a cubic spline follows its tangents") {
  const ModelData data = load("models/skinned.gltf");
  const auto root_at = [&](std::string_view clip, double time) {
    const auto skins = vnmaki::skin_matrices(data, pose_at(data, clip, time));
    return vnmaki::transform_point(skins.at(0).at(0), {.x = 0, .y = 0, .z = 0});
  };
  check_point(root_at("snap", 0.99), {.x = 0, .y = 0, .z = 0});
  check_point(root_at("snap", kSecond), {.x = 0, .y = 1, .z = 0});
  check_point(root_at("snap", 1.5), {.x = 0, .y = 1, .z = 0});
  check_point(root_at("snap", 2.0), {.x = 0, .y = 2, .z = 0});
  check_point(root_at("slide", 0), {.x = 0, .y = 0, .z = 0});
  check_point(root_at("slide", kQuarterSecond), {.x = kSlideQuarter, .y = 0, .z = 0});
  check_point(root_at("slide", kHalfSecond), {.x = kSlideHalf, .y = 0, .z = 0});
  check_point(root_at("slide", kSecond), {.x = 1, .y = 0, .z = 0});
}

TEST_CASE("anim: posed vertices are skinned by weight, and the normals stay unit length") {
  const ModelData data = load("models/skinned.gltf");
  const vnmaki::ModelPose pose = pose_at(data, "bend", kSecond);
  const auto skins = vnmaki::skin_matrices(data, pose);
  std::vector<vnmaki::MeshVertex> vertices;
  vnmaki::pose_vertices(data.meshes.front(), pose, skins, vertices);
  REQUIRE(vertices.size() == kBarVertices);
  // The bottom ring follows the root alone, the top ring the tip alone.
  CHECK(vertices.at(0).position == std::array<float, 3>{-kBarHalf, 0, -kBarHalf});
  check_point({.x = vertices.at(kTopRingVertex).position.at(0),
               .y = vertices.at(kTopRingVertex).position.at(1),
               .z = vertices.at(kTopRingVertex).position.at(2)},
              {.x = -1, .y = 1 - kBarHalf, .z = -kBarHalf});
  // The middle ring is half way between where each joint would put it.
  check_point({.x = vertices.at(kMiddleRingVertex).position.at(0),
               .y = vertices.at(kMiddleRingVertex).position.at(1),
               .z = vertices.at(kMiddleRingVertex).position.at(2)},
              {.x = -kBarHalf / 2, .y = 1 - (kBarHalf / 2), .z = -kBarHalf});
  for (const vnmaki::MeshVertex& vertex : vertices) {
    const float length = std::sqrt((vertex.normal.at(0) * vertex.normal.at(0)) +
                                   (vertex.normal.at(1) * vertex.normal.at(1)) +
                                   (vertex.normal.at(2) * vertex.normal.at(2)));
    CHECK(length == doctest::Approx(1.0F).epsilon(kTolerance));
  }
  // The bent normal of the top ring turned with the tip: (-k, 0, -k) becomes (0, -k, -k).
  CHECK(vertices.at(kTopRingVertex).normal.at(0) == doctest::Approx(0.0F).epsilon(kTolerance));
  CHECK(vertices.at(kTopRingVertex).normal.at(1) == doctest::Approx(-kRoot2Half).epsilon(kTolerance));
}

// --- Morph targets ---------------------------------------------------------------------------

TEST_CASE("anim: morph weights follow the clip and move the vertices") {
  const ModelData data = load("models/morph.gltf");
  const vnmaki::ModelMesh& quad = data.meshes.front();
  const vnmaki::ModelPose half = pose_at(data, "open", kHalfSecond);
  CHECK(half.weights.at(0) == std::vector<float>{0.5F, 0.25F});
  std::vector<vnmaki::MeshVertex> vertices;
  vnmaki::pose_vertices(quad, half, {}, vertices);
  CHECK(vertices.at(2).position == std::array<float, 3>{1, 1.5F, 0});      // half of mouth_open
  CHECK(vertices.at(0).position == std::array<float, 3>{-1.125F, 0, 0});  // quarter of smile
  // Without a clip the weights of the file (0) leave the quad alone.
  vnmaki::pose_vertices(quad, vnmaki::rest_pose(data), {}, vertices);
  CHECK(vertices.at(2).position == quad.vertices.at(2).position);
  // Lip sync sets a target by name; an unknown name sets nothing.
  vnmaki::ModelPose pose = vnmaki::rest_pose(data);
  CHECK(vnmaki::set_morph_weight(data, pose, "mouth_open", kOpenWeight));
  CHECK_FALSE(vnmaki::set_morph_weight(data, pose, "frown", 1));
  vnmaki::pose_vertices(quad, pose, {}, vertices);
  CHECK(vertices.at(3).position.at(1) == doctest::Approx(1 + kOpenWeight));
}

// --- The player: crossfades, loops, ends -----------------------------------------------------

TEST_CASE("anim: a crossfade blends the old clip into the new one by weight") {
  const ModelData data = load("models/skinned.gltf");
  ClipPlayer player;
  vnmaki::start_clip(player, "bend", PlayOptions{.seek = kSecond});  // the tip is turned 90
  vnmaki::start_clip(player, "flip", PlayOptions{.fade = kSecond});  // to -90 in a second
  REQUIRE(player.layers.size() == 2);
  CHECK(vnmaki::layer_weight(player.layers.back()) == 0);
  check_point(tip_point(data, vnmaki::evaluate_pose(data, player), 2), {.x = -1, .y = 1, .z = 0});

  vnmaki::advance_clips(player, data, kQuarterSecond);
  CHECK(vnmaki::layer_weight(player.layers.back()) == doctest::Approx(kQuarterSecond));
  // A quarter of the way from 90 to -90 degrees is 45 degrees.
  check_point(tip_point(data, vnmaki::evaluate_pose(data, player), 2),
              {.x = -kRoot2Half, .y = 1 + kRoot2Half, .z = 0});
  vnmaki::advance_clips(player, data, kQuarterSecond);
  CHECK(vnmaki::layer_weight(player.layers.back()) == doctest::Approx(kHalfSecond));
  check_point(tip_point(data, vnmaki::evaluate_pose(data, player), 2), {.x = 0, .y = 2, .z = 0});
  CHECK(vnmaki::clips_busy(player, data));

  vnmaki::advance_clips(player, data, kHalfSecond);
  // The new clip has faded in: the old one is gone and the tip is at -90 degrees.
  REQUIRE(player.layers.size() == 1);
  CHECK(player.layers.front().clip == "flip");
  check_point(tip_point(data, vnmaki::evaluate_pose(data, player), 2), {.x = 1, .y = 1, .z = 0});
  CHECK_FALSE(vnmaki::clips_busy(player, data));  // "flip" is at its end
}

TEST_CASE("anim: the first clip fades in from the pose at rest") {
  const ModelData data = load("models/skinned.gltf");
  ClipPlayer player;
  vnmaki::start_clip(player, "flip", PlayOptions{.fade = kSecond});
  vnmaki::advance_clips(player, data, kHalfSecond);
  // Half way from 0 to -90 degrees: the point 1 above the joint turns 45 degrees to the right.
  check_point(tip_point(data, vnmaki::evaluate_pose(data, player), 2),
              {.x = kRoot2Half, .y = 1 + kRoot2Half, .z = 0});
}

TEST_CASE("anim: a new play during a fade stacks, and the oldest clips drop out") {
  const ModelData data = load("models/skinned.gltf");
  ClipPlayer player;
  for (std::size_t i = 0; i < kManyPlays; ++i) {
    vnmaki::start_clip(player, i % 2 == 0 ? "bend" : "flip", PlayOptions{.fade = kSecond});
  }
  CHECK(player.layers.size() == vnmaki::kMaxClipLayers);
  vnmaki::advance_clips(player, data, kQuarterSecond);
  CHECK(player.layers.size() == vnmaki::kMaxClipLayers);  // none has faded in
  vnmaki::advance_clips(player, data, kSecond);
  CHECK(player.layers.size() == 1);  // the newest has faded in fully and covers the others
}

TEST_CASE("anim: a loop wraps, speed scales time, and a clip that ends stays at its end") {
  const ModelData data = load("models/skinned.gltf");
  ClipPlayer looped;
  vnmaki::start_clip(looped, "bend", PlayOptions{.loop = true, .speed = 2});
  vnmaki::advance_clips(looped, data, 0.75);  // 1.5 s into a 1 s clip
  CHECK(looped.layers.front().time == doctest::Approx(kHalfSecond));
  CHECK_FALSE(vnmaki::clips_busy(looped, data));  // a loop never blocks a wait

  ClipPlayer once;
  vnmaki::start_clip(once, "bend", PlayOptions{.speed = 2});
  CHECK(vnmaki::clips_busy(once, data));
  vnmaki::advance_clips(once, data, kQuarterSecond);
  CHECK(vnmaki::clips_busy(once, data));
  vnmaki::advance_clips(once, data, kQuarterSecond);
  CHECK(once.layers.front().time == doctest::Approx(kSecond));
  CHECK_FALSE(vnmaki::clips_busy(once, data));
  vnmaki::advance_clips(once, data, kSecond);
  CHECK(once.layers.front().time == doctest::Approx(kSecond));

  // finish puts a clip at its end, and leaves a loop going.
  ClipPlayer skipped;
  vnmaki::start_clip(skipped, "bend", {});
  vnmaki::finish_clips(skipped);
  CHECK(skipped.layers.front().time == vnmaki::kEndOfClip);
  CHECK_FALSE(vnmaki::clips_busy(skipped, data));
  vnmaki::finish_clips(looped);
  CHECK(looped.layers.front().time == doctest::Approx(kHalfSecond));
}

TEST_CASE("anim: a replay or a snapshot puts a clip at its end, and a loop at time 0") {
  const ModelData data = load("models/skinned.gltf");
  ClipPlayer replayed;
  vnmaki::start_clip(replayed, "bend", PlayOptions{.fade = kSecond, .replaying = true});
  REQUIRE(replayed.layers.size() == 1);  // no fade in a replay
  CHECK(replayed.layers.front().time == vnmaki::kEndOfClip);
  check_point(tip_point(data, vnmaki::evaluate_pose(data, replayed), 2), {.x = -1, .y = 1, .z = 0});
  ClipPlayer looped;
  vnmaki::start_clip(looped, "bend", PlayOptions{.loop = true, .seek = kHalfSecond, .replaying = true});
  CHECK(looped.layers.front().time == 0);

  ClipPlayer live;
  vnmaki::start_clip(live, "flip", {});
  vnmaki::start_clip(live, "bend", PlayOptions{.loop = true, .fade = kSecond, .speed = 3});
  vnmaki::advance_clips(live, data, kQuarterSecond);
  const ClipPlayer settled = vnmaki::settled(live);
  REQUIRE(settled.layers.size() == 1);
  CHECK(settled.layers.front().clip == "bend");
  CHECK(settled.layers.front().time == 0);
  CHECK(settled.layers.front().speed == 3);
  CHECK(settled.layers.front().fade == 0);
  CHECK(vnmaki::settled(ClipPlayer{}).layers.empty());
}

// --- Scripts ---------------------------------------------------------------------------------

vnmaki::Vfs game_files(const std::string& script) {
  vnmaki::Vfs vfs = fixture_vfs();
  vfs.mount(
      std::make_unique<vnmaki::test::MemoryMount>(std::map<std::string, std::string, std::less<>>{
          {"scripts/main.lua", script}, {"images/eileen.png", ""}}));
  return vfs;
}

struct Stage {
  explicit Stage(const std::string& script)
      : vfs(game_files(script)), scene(vfs, kVirtualSize), runtime(vfs, options()) {
    runtime.load_scripts();
    runtime.start("start");
  }

  [[nodiscard]] vnmaki::ScriptRuntimeOptions options() {
    vnmaki::ScriptRuntimeOptions result;
    result.scene_state = &scene;
    result.scene = &scene;
    return result;
  }
  [[nodiscard]] std::string text() const {
    const auto* narrate = std::get_if<vnmaki::NarrateRequest>(runtime.request());
    return narrate != nullptr ? narrate->text : std::string{};
  }
  [[nodiscard]] Scene3dDisplayable* stage() {
    return dynamic_cast<Scene3dDisplayable*>(scene.find("stage"));
  }
  [[nodiscard]] vnmaki::Model3d& model(std::size_t index = 0) {
    return stage()->models().at(index);
  }

  vnmaki::Vfs vfs;
  Scene scene;
  vnmaki::ScriptRuntime runtime;
};

constexpr std::string_view kBendAndWait = R"(
label("start", function()
  scene3d("stage")
  model("arm", "models/skinned.gltf", { scene = "stage" })
  play("@3d.stage.arm", "bend", { speed = 2 })
  wait("@3d.stage.arm")
  narrate("done")
end)
)";

TEST_CASE("script: play starts a clip and wait ends when a clip that does not loop has ended") {
  Stage stage{std::string{kBendAndWait}};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  vnmaki::WaitPresenter waits{stage.runtime, stage.scene};
  REQUIRE(waits.waiting());
  CHECK(stage.scene.tweening("@3d.stage.arm"));
  CHECK(stage.scene.tweening(""));  // wait_all waits for it too
  CHECK_FALSE(stage.scene.tweening("@3d.stage.other"));
  stage.scene.update(kQuarterSecond);  // 0.5 s into a 1 s clip, at speed 2
  CHECK_FALSE(waits.update());
  CHECK(stage.model().clips.layers.front().time == doctest::Approx(kHalfSecond));
  stage.scene.update(kQuarterSecond);
  REQUIRE(waits.update());
  CHECK(stage.text() == "done");
  CHECK_FALSE(stage.scene.tweening("@3d.stage.arm"));
}

TEST_CASE("script: a click ends the waited clip at its end") {
  Stage stage{std::string{kBendAndWait}};
  vnmaki::WaitPresenter waits{stage.runtime, stage.scene};
  REQUIRE(waits.skip());
  CHECK(stage.text() == "done");
  CHECK(stage.model().clips.layers.front().time == vnmaki::kEndOfClip);
  CHECK_FALSE(stage.scene.tweening("@3d.stage.arm"));
}

TEST_CASE("script: wait does not block on a loop") {
  Stage stage{R"(
label("start", function()
  scene3d("stage")
  model("arm", "models/skinned.gltf", { scene = "stage" })
  play("@3d.stage.arm", "bend", { loop = true, fade = 0.5, seek = 0.25 })
  wait("@3d.stage.arm")
  narrate("done")
end)
)"};
  vnmaki::WaitPresenter waits{stage.runtime, stage.scene};
  REQUIRE(waits.update());
  CHECK(stage.text() == "done");
  CHECK(stage.model().clips.layers.back().loop);
  CHECK(stage.model().clips.layers.back().time == doctest::Approx(kQuarterSecond));
}

// The error of the story whose `start` label has a model "arm" (skinned bar) and runs `lines`.
std::string line_error(const std::string& lines) {
  Stage stage{"label(\"start\", function()\n  scene3d(\"stage\")\n"
              "  model(\"arm\", \"models/skinned.gltf\", { scene = \"stage\" })\n" +
              lines + "\nend)\n"};
  REQUIRE(stage.runtime.state() == vnmaki::RunState::kFailed);
  return stage.runtime.error();
}

TEST_CASE("script: play and lip_sync report their mistakes at the script line") {
  const auto has = [](const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
  };
  CHECK(has(line_error(R"(play("@3d.stage.arm", "nope"))"), "has no clip 'nope'"));
  CHECK(has(line_error(R"(play("@3d.stage.arm", "nope"))"), "bend, flip, snap, slide"));
  CHECK(has(line_error(R"(play("arm", "bend"))"), "@3d.<scene>.<model>"));
  CHECK(has(line_error(R"(play("@3d.stage.ghost", "bend"))"), "no model 'ghost'"));
  CHECK(has(line_error(R"(play("@3d.nowhere.arm", "bend"))"), "no 3D scene 'nowhere'"));
  CHECK(has(line_error(R"(play("@3d.stage.arm", 5))"), "clip name"));
  CHECK(has(line_error(R"(play("@3d.stage.arm", "bend", { fade = -1 }))"), "'fade'"));
  CHECK(has(line_error(R"(play("@3d.stage.arm", "bend", { speed = 0 }))"), "'speed'"));
  CHECK(has(line_error(R"(play("@3d.stage.arm", "bend", { seek = -1 }))"), "'seek'"));
  CHECK(has(line_error(R"(play("@3d.stage.arm", "bend", { loop = 1 }))"), "'loop'"));
  CHECK(has(line_error(R"(play("@3d.stage.arm", "bend", { looping = true }))"), "'looping'"));
  CHECK(has(line_error(R"(model("m", "models/morph.gltf", { scene = "stage",
                                    lip_sync = { morph = "frown" } }))"),
            "no morph target 'frown'"));
  CHECK(has(line_error(R"(model("m", "models/morph.gltf", { scene = "stage",
                                    lip_sync = { morph = 5 } }))"),
            "'morph'"));
  CHECK(has(line_error(R"(model("m", "models/morph.gltf", { scene = "stage",
                                    lip_sync = { morph = "smile", group = "x" } }))"),
            "unknown key 'group'"));
  CHECK(has(line_error(R"(model("m", "models/morph.gltf", { scene = "stage", lip_sync = 1 }))"),
            "'lip_sync'"));
}

TEST_CASE("script: a model with another file loses the clips of the old one") {
  Stage stage{R"(
label("start", function()
  scene3d("stage")
  model("arm", "models/skinned.gltf", { scene = "stage" })
  play("@3d.stage.arm", "bend")
  model("arm", "models/cube.gltf", { scene = "stage" })
  narrate("done")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  CHECK(stage.model().clips.layers.empty());
}

// --- Lip sync --------------------------------------------------------------------------------

class FakeVoice final : public vnmaki::VoiceLevel {
 public:
  [[nodiscard]] std::optional<double> voice_level(std::string_view tag) const override {
    return tag == playing ? std::optional<double>{level} : std::nullopt;
  }

  std::string playing{};
  double level = 0;
};

constexpr std::string_view kLipSync = R"(
label("start", function()
  scene3d("stage")
  model("mouth", "models/morph.gltf", { scene = "stage", lip_sync = { morph = "mouth_open" } })
  model("other", "models/morph.gltf", { scene = "stage",
                                        lip_sync = { morph = "smile", voice_tag = "alice" } })
  narrate("talking")
end)
)";

TEST_CASE("lip sync: the morph target follows the voice level of the model's voice tag") {
  Stage stage{std::string{kLipSync}};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  CHECK(stage.model(0).lip_voice_tag == "mouth");  // the model's tag unless the script says
  CHECK(stage.model(1).lip_voice_tag == "alice");
  FakeVoice voice;
  stage.scene.set_voice_level(&voice);
  const ModelData data = load("models/morph.gltf");
  const auto weights = [&](std::size_t model) {
    return vnmaki::pose_of(stage.model(model), data).weights.at(0);
  };

  stage.scene.update(0.01);
  CHECK(weights(0) == std::vector<float>{0, 0});  // nobody speaks
  voice.playing = "mouth";
  voice.level = kVoiceLevel;
  stage.scene.update(0.01);
  CHECK(stage.model(0).lip_level == doctest::Approx(kVoiceLevel));
  CHECK(weights(0).at(0) == doctest::Approx(static_cast<float>(kVoiceLevel)));
  CHECK(weights(0).at(1) == 0);
  CHECK(weights(1) == std::vector<float>{0, 0});  // the other model follows "alice"
  voice.playing = "alice";
  voice.level = 2.0;  // a level is clamped to 1
  stage.scene.update(0.01);
  CHECK(weights(1).at(1) == 1);
  voice.playing = "";
  stage.scene.update(0.01);
  CHECK(weights(1) == std::vector<float>{0, 0});  // the mouth closes when the voice ends
}

// --- The snapshot and the replay -------------------------------------------------------------

constexpr std::string_view kPlaying = R"(
label("start", function()
  scene3d("stage")
  model("arm", "models/skinned.gltf", { scene = "stage" })
  model("mouth", "models/morph.gltf", { scene = "stage", lip_sync = { morph = "smile",
                                                                       voice_tag = "bob" } })
  model("idle", "models/skinned.gltf", { scene = "stage" })
  play("@3d.stage.arm", "bend", { speed = 2, seek = 0.25 })
  play("@3d.stage.idle", "slide", { loop = true, fade = 1 })
  narrate("playing")
end)
)";

TEST_CASE("snapshot: a clip is kept at its end, a loop at time 0, and the lip sync as it was") {
  Stage stage{std::string{kPlaying}};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  stage.scene.update(kQuarterSecond);  // the clips are in the middle
  CHECK(stage.model(0).clips.layers.front().time < 1);
  const std::vector<std::byte> bytes = stage.scene.capture();

  stage.scene.restore({});
  CHECK(stage.stage() == nullptr);
  stage.scene.restore(bytes);
  REQUIRE(stage.stage() != nullptr);
  const auto& arm = stage.model(0).clips.layers;
  REQUIRE(arm.size() == 1);
  CHECK(arm.front().clip == "bend");
  CHECK(arm.front().time == vnmaki::kEndOfClip);
  CHECK(arm.front().speed == 2);
  CHECK_FALSE(arm.front().loop);
  CHECK(stage.model(1).clips.layers.empty());
  CHECK(stage.model(1).lip_morph == "smile");
  CHECK(stage.model(1).lip_voice_tag == "bob");
  CHECK(stage.model(1).lip_level == 0);  // the mouth is closed after a restore
  const auto& idle = stage.model(2).clips.layers;
  REQUIRE(idle.size() == 1);  // no fade: the loop is the only clip
  CHECK(idle.front().clip == "slide");
  CHECK(idle.front().loop);
  CHECK(idle.front().time == 0);
  CHECK(stage.scene.capture() == bytes);
  CHECK_FALSE(stage.scene.tweening("@3d.stage.arm"));  // it is at its end
  stage.scene.update(kQuarterSecond);
  CHECK(stage.model(2).clips.layers.front().time == doctest::Approx(kQuarterSecond));
}

TEST_CASE("snapshot: a refused clip speed changes nothing") {
  Stage stage{std::string{kPlaying}};
  std::vector<std::byte> bytes = stage.scene.capture();
  // The speed of the "bend" clip is the f32 after the name and the loop flag: make it a NaN.
  const std::array<std::byte, 4> name{std::byte{'b'}, std::byte{'e'}, std::byte{'n'},
                                      std::byte{'d'}};
  const auto found = std::search(bytes.begin(), bytes.end(), name.begin(), name.end());
  REQUIRE(found != bytes.end());
  constexpr std::ptrdiff_t kSpeedOffset = 5;  // "bend", then one byte
  const std::array<std::byte, 4> not_a_number{std::byte{0}, std::byte{0}, std::byte{0xC0},
                                              std::byte{0x7F}};
  std::copy(not_a_number.begin(), not_a_number.end(), std::next(found, kSpeedOffset));
  Scene other{stage.vfs, kVirtualSize};
  CHECK_THROWS_AS(other.restore(bytes), std::invalid_argument);
  CHECK(other.tags(vnmaki::Layer::k3d).empty());
}

TEST_CASE("replay: a replay puts a clip at its end and a loop at time 0") {
  const std::string story = R"(
label("start", function()
  scene3d("stage")
  model("arm", "models/skinned.gltf", { scene = "stage" })
  model("idle", "models/skinned.gltf", { scene = "stage" })
  play("@3d.stage.arm", "bend", { fade = 2 })
  play("@3d.stage.idle", "flip", { loop = true, seek = 0.5 })
  wait("@3d.stage.arm")
  narrate("one")
  narrate("two")
end)
)";
  Stage live{story};
  vnmaki::WaitPresenter waits{live.runtime, live.scene};
  live.scene.update(2.0);  // the fade is over and the clip has ended
  REQUIRE(waits.update());
  REQUIRE(live.text() == "one");
  REQUIRE(live.runtime.label_snapshot() != nullptr);
  const vnmaki::LabelSnapshot snapshot = *live.runtime.label_snapshot();
  const std::vector<vnmaki::InputResult> inputs{live.runtime.input_log().begin(),
                                                live.runtime.input_log().end()};
  const std::vector<std::byte> expected = live.scene.capture();

  Stage fresh{story};
  fresh.runtime.replay(snapshot, inputs, 2);  // the line after the wait
  REQUIRE_MESSAGE(fresh.runtime.state() == vnmaki::RunState::kWaiting, fresh.runtime.error());
  CHECK(fresh.text() == "one");
  const auto& arm = fresh.model(0).clips.layers;
  REQUIRE(arm.size() == 1);
  CHECK(arm.front().time == vnmaki::kEndOfClip);
  CHECK(fresh.model(1).clips.layers.front().time == 0);
  CHECK_FALSE(fresh.scene.tweening("@3d.stage.arm"));
  CHECK(fresh.scene.capture() == expected);
}

TEST_CASE("scene: removing a model takes its clip out of the wait") {
  Stage stage{std::string{kPlaying}};
  REQUIRE(stage.stage() != nullptr);
  CHECK(stage.stage()->remove("arm"));
  CHECK_FALSE(stage.scene.tweening("@3d.stage.arm"));
  CHECK(stage.stage()->models().size() == 2);
}

}  // namespace
