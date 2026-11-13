// 3D scenes (docs/SPEC.md section 12): the glTF loader (on the cubes of
// tools/generators/model_fixtures.py), the maths, the script calls `scene3d`, `model`, `light`,
// `camera(scene, ...)` and `hide3d`, tweens and waits on "@3d.<scene>.<name>", and the snapshot.
// Time is a fake clock. The frames are checked on a GPU in tests/render/model3d_render_test.cpp.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/anim/wait_presenter.hpp"
#include "engine/model3d/math3d.hpp"
#include "engine/model3d/model.hpp"
#include "engine/model3d/scene3d.hpp"
#include "engine/render/shader_compiler.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"
#include "fuzz/model_properties.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::Layer;
using vnmaki::ModelData;
using vnmaki::Scene;
using vnmaki::Scene3dDisplayable;
using vnmaki::WaitPresenter;

constexpr vnmaki::Size kVirtualSize{.width = 64, .height = 64};
constexpr double kHalfSecond = 0.5;
constexpr double kSecond = 1.0;
constexpr float kTolerance = 1e-4F;
constexpr std::size_t kCubeVertices = 24;
constexpr std::size_t kCubeIndices = 36;
constexpr float kOrangeRed = 0.8F;
constexpr float kOrangeGreen = 0.4F;
constexpr float kOrangeBlue = 0.2F;
constexpr float kHalfRoughness = 0.5F;
constexpr float kQuarterTurn = 90.0F;
constexpr float kFov = 50.0F;
constexpr int kMutations = 300;
constexpr std::uint32_t kSeed = 46;
constexpr std::size_t kCutStep = 7;
constexpr float kFarAway = 1e9F;
// moved.gltf: the cube is translated by 10 and scaled by 2.
constexpr float kMovedLowX = 8.0F;
constexpr float kMovedHighX = 12.0F;
constexpr float kMovedTop = 2.0F;
// The camera of the maths tests: 90 degrees, so the half-width at distance d is d.
constexpr float kRightFov = 90.0F;
constexpr float kBesideDistance = 4.0F;
constexpr float kBesideX = 0.25F;
constexpr float kBesideY = 0.125F;
constexpr float kFiveBehind = -5.0F;
constexpr float kGreenOfOrange = 128.0F / 255.0F;
// Half way through the one second tweens of kTweened.
constexpr float kHalfY = 1.0F;
constexpr float kHalfTurn = 45.0F;
constexpr float kHalfScale = 2.0F;
constexpr float kHalfCameraZ = 7.0F;
constexpr float kHalfFov = 45.0F;
constexpr float kHalfIntensity = 0.5F;
constexpr float kHalfwayAmount = 0.625F;
constexpr float kLoopHalfway = 5.5F;
constexpr double kShortWhile = 0.1;
constexpr float kStartAmount = 0.25F;  // the amount of the whiten shader in its script
// The channels of a pose (see kModelChannels, kLightChannels and kCameraChannels).
constexpr std::size_t kChannelY = 1;
constexpr std::size_t kChannelZ = 2;
constexpr std::size_t kChannelRy = 4;
constexpr std::size_t kChannelScale = 6;
constexpr std::size_t kChannelIntensity = 6;
constexpr std::size_t kChannelFov = 6;

vnmaki::Vfs fixture_vfs() {
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_FIXTURES_DIR} /
                                                 "model3d" / "game")));
  return vfs;
}

// The fixture game with a `main.lua` of the test's own on top, and an image to show.
vnmaki::Vfs game_files(const std::string& script) {
  vnmaki::Vfs vfs = fixture_vfs();
  vfs.mount(
      std::make_unique<vnmaki::test::MemoryMount>(std::map<std::string, std::string, std::less<>>{
          {"scripts/main.lua", script}, {"images/eileen.png", ""}}));
  return vfs;
}

// --- The loader ------------------------------------------------------------------------------

void check_cube(const ModelData& model) {
  REQUIRE(model.meshes.size() == 1);
  const vnmaki::ModelMesh& mesh = model.meshes.front();
  CHECK(mesh.vertices.size() == kCubeVertices);
  CHECK(mesh.indices.size() == kCubeIndices);
  CHECK(vnmaki::fuzz::model_is_consistent(model));
  REQUIRE(model.materials.size() >= 1);
  const vnmaki::ModelMaterial& material = model.materials.at(mesh.material);
  CHECK(material.color.at(0) == doctest::Approx(kOrangeRed));
  CHECK(material.color.at(1) == doctest::Approx(kOrangeGreen));
  CHECK(material.color.at(2) == doctest::Approx(kOrangeBlue));
  CHECK(material.color.at(3) == doctest::Approx(1.0F));
  CHECK(material.metallic == doctest::Approx(0.0F));
  CHECK(material.roughness == doctest::Approx(kHalfRoughness));
  // The first face is +z: its normal points at the camera and its corners are at z = 1.
  const vnmaki::MeshVertex& first = mesh.vertices.front();
  CHECK(first.normal == std::array<float, 3>{0, 0, 1});
  CHECK(first.position.at(2) == 1);
}

TEST_CASE("model3d: a .gltf cube loads with its material, normals and texture coordinates") {
  const vnmaki::Vfs vfs = fixture_vfs();
  const ModelData model = vnmaki::load_model(vfs, "models/cube.gltf");
  check_cube(model);
  CHECK(model.images.empty());
  CHECK(model.materials.at(model.meshes.front().material).image == -1);
  const vnmaki::MeshVertex& corner = model.meshes.front().vertices.front();
  CHECK(corner.uv == std::array<float, 2>{0, 1});
}

TEST_CASE("model3d: a .glb cube is the same as the .gltf") {
  const vnmaki::Vfs vfs = fixture_vfs();
  const ModelData gltf = vnmaki::load_model(vfs, "models/cube.gltf");
  const ModelData glb = vnmaki::load_model(vfs, "models/cube.glb");
  check_cube(glb);
  REQUIRE(glb.meshes.size() == gltf.meshes.size());
  CHECK(glb.meshes.front().indices == gltf.meshes.front().indices);
  for (std::size_t i = 0; i < kCubeVertices; ++i) {
    CHECK(glb.meshes.front().vertices.at(i).position ==
          gltf.meshes.front().vertices.at(i).position);
  }
}

TEST_CASE("model3d: a texture comes from a file next to the model or from the .glb") {
  const vnmaki::Vfs vfs = fixture_vfs();
  for (const char* path : {"models/tex.gltf", "models/tex.glb"}) {
    CAPTURE(path);
    const ModelData model = vnmaki::load_model(vfs, path);
    REQUIRE(model.images.size() == 1);
    CHECK(model.materials.at(model.meshes.front().material).image == 0);
    const vnmaki::Image& image = model.images.front();
    CHECK(image.size == vnmaki::Size{.width = 2, .height = 2});
    CHECK(image.pixels.at(0) == UINT8_MAX);  // the top-left texel is red
    CHECK(image.pixels.at(1) == 0);
    CHECK(image.pixels.at(5) == UINT8_MAX);  // the top-right texel is green
  }
}

TEST_CASE("model3d: the transform of a node is applied to its mesh") {
  const vnmaki::Vfs vfs = fixture_vfs();
  const ModelData model = vnmaki::load_model(vfs, "models/moved.gltf");
  float low_x = kFarAway;
  float high_x = -kFarAway;
  float high_y = -kFarAway;
  for (const vnmaki::MeshVertex& vertex : model.meshes.front().vertices) {
    low_x = std::min(low_x, vertex.position.at(0));
    high_x = std::max(high_x, vertex.position.at(0));
    high_y = std::max(high_y, vertex.position.at(1));
    const float length = std::sqrt((vertex.normal.at(0) * vertex.normal.at(0)) +
                                   (vertex.normal.at(1) * vertex.normal.at(1)) +
                                   (vertex.normal.at(2) * vertex.normal.at(2)));
    CHECK(length == doctest::Approx(1.0F));
  }
  CHECK(low_x == doctest::Approx(kMovedLowX));
  CHECK(high_x == doctest::Approx(kMovedHighX));
  CHECK(high_y == doctest::Approx(kMovedTop));
}

TEST_CASE("model3d: a file that cannot be read is an error that names the file") {
  const vnmaki::Vfs vfs = fixture_vfs();
  const std::map<std::string, std::string> expected{
      {"models/absent.gltf", "missing"},
      {"models/bad/missing_bin.gltf", "missing"},
      {"models/bad/missing_texture.gltf", "image 'absent.png' is missing"},
      {"models/bad/lines.gltf", "triangle"},
      {"models/bad/not_json.gltf", "models/bad/not_json.gltf"},
      {"models/bad/truncated.glb", "models/bad/truncated.glb"},
      {"models/bad/index_range.gltf", "models/bad/index_range.gltf"},
  };
  for (const auto& [path, reason] : expected) {
    CAPTURE(path);
    try {
      static_cast<void>(vnmaki::load_model(vfs, path));
      FAIL("no error");
    } catch (const vnmaki::ModelError& error) {
      const std::string_view message = error.what();
      CHECK(message.find("model '" + path + "'") != std::string_view::npos);
      CHECK(message.find(reason) != std::string_view::npos);
    }
  }
}

TEST_CASE("model3d: an accessor with a huge count is refused before anything is allocated") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({{"huge.gltf", R"({
    "asset": { "version": "2.0" }, "scenes": [{ "nodes": [0] }], "nodes": [{ "mesh": 0 }],
    "meshes": [{ "primitives": [{ "attributes": { "POSITION": 0 } }] }],
    "accessors": [{ "componentType": 5126, "count": 4000000000, "type": "VEC3" }] })"}});
  CHECK_THROWS_AS(static_cast<void>(vnmaki::load_model(vfs, "huge.gltf")), vnmaki::ModelError);
}

TEST_CASE("model3d: a damaged cube loads or fails, and never draws out of range") {
  const vnmaki::Vfs vfs = fixture_vfs();
  const std::optional<std::vector<std::byte>> original = vfs.read("models/cube.glb");
  REQUIRE(original.has_value());
  std::mt19937 random{kSeed};  // NOLINT(bugprone-random-generator-seed)
  int loaded = 0;
  int refused = 0;
  for (int i = 0; i < kMutations; ++i) {
    std::vector<std::byte> bytes = *original;
    for (int edit = 0; edit < 4; ++edit) {
      bytes.at(random() % bytes.size()) = static_cast<std::byte>(random());
    }
    try {
      const ModelData model = vnmaki::parse_model(vfs, "models/cube.glb", bytes);
      CHECK(vnmaki::fuzz::model_is_consistent(model));
      ++loaded;
    } catch (const vnmaki::ModelError&) {
      ++refused;  // A refused file is fine.
    }
  }
  CHECK(loaded > 0);
  CHECK(refused > 0);
}

// --- The maths -------------------------------------------------------------------------------

struct Clip {
  float x = 0;
  float y = 0;
  float z = 0;
  float w = 0;
};

Clip apply(const vnmaki::Mat4& m, vnmaki::Vec3 v) {
  const auto row = [&](std::size_t r) {
    return (m.at(r * 4) * v.x) + (m.at((r * 4) + 1) * v.y) + (m.at((r * 4) + 2) * v.z) +
           m.at((r * 4) + 3);
  };
  return {.x = row(0), .y = row(1), .z = row(2), .w = row(3)};
}

TEST_CASE("model3d: the camera maps a point in front of it to clip space with depth 0 to 1") {
  const vnmaki::Mat4 view = vnmaki::look_at({.x = 0, .y = 0, .z = 5}, {.x = 0, .y = 0, .z = 0});
  const vnmaki::Mat4 both = vnmaki::multiply(vnmaki::perspective(kRightFov, 1.0F), view);
  // With a field of view of 90 degrees the half-width at distance d is d.
  const Clip beside = apply(both, {.x = 1, .y = 0.5F, .z = 1});  // distance 4
  CHECK(beside.w == doctest::Approx(kBesideDistance));
  CHECK(beside.x / beside.w == doctest::Approx(kBesideX));
  CHECK(beside.y / beside.w == doctest::Approx(kBesideY));
  const Clip near_plane = apply(both, {.x = 0, .y = 0, .z = 5 - vnmaki::kNearPlane});
  CHECK(near_plane.z / near_plane.w == doctest::Approx(0.0F).epsilon(kTolerance));
  const Clip far_plane = apply(both, {.x = 0, .y = 0, .z = 5 - vnmaki::kFarPlane});
  CHECK(far_plane.z / far_plane.w == doctest::Approx(1.0F).epsilon(kTolerance));
  // Up is up: a point above the centre has a positive y.
  CHECK(apply(both, {.x = 0, .y = 1, .z = 0}).y > 0);
}

TEST_CASE("model3d: look_at survives a camera that looks straight down or at itself") {
  const vnmaki::Mat4 down = vnmaki::look_at({.x = 0, .y = 5, .z = 0}, {.x = 0, .y = 0, .z = 0});
  const Clip below = apply(down, {.x = 0, .y = 0, .z = 0});
  CHECK(below.z == doctest::Approx(kFiveBehind));  // in front of the camera, in view space
  for (const float value : vnmaki::look_at({.x = 1, .y = 1, .z = 1}, {.x = 1, .y = 1, .z = 1})) {
    CHECK(std::isfinite(value));
  }
}

TEST_CASE("model3d: the model matrix scales, turns about y and moves") {
  const vnmaki::Mat4 m =
      vnmaki::model_matrix({.x = 1, .y = 2, .z = 3}, {.x = 0, .y = kQuarterTurn, .z = 0}, 2.0F);
  const Clip moved = apply(m, {.x = 1, .y = 0, .z = 0});
  CHECK(moved.x == doctest::Approx(1.0F).epsilon(kTolerance));
  CHECK(moved.y == doctest::Approx(kMovedTop));
  CHECK(moved.z == doctest::Approx(1.0F).epsilon(kTolerance));  // x turns to -z: 3 - 2
}

// --- The script ------------------------------------------------------------------------------

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
  [[nodiscard]] Scene3dDisplayable* stage(std::string_view tag = "stage") {
    return dynamic_cast<Scene3dDisplayable*>(scene.find(tag));
  }

  vnmaki::Vfs vfs;
  Scene scene;
  vnmaki::ScriptRuntime runtime;
};

// The error of the story whose `start` label runs `lines`, after a scene named "stage" is made.
std::string line_error(const std::string& lines) {
  const Stage stage{"label(\"start\", function()\n  scene3d(\"stage\")\n  show(\"eileen\")\n" +
                    lines + "\nend)\n"};
  REQUIRE(stage.runtime.state() == vnmaki::RunState::kFailed);
  return stage.runtime.error();
}

constexpr std::string_view kBuilt = R"(
label("start", function()
  scene3d("stage")
  camera("stage", { position = { 0, 1, 6 }, look_at = { 0, 1, 0 }, fov = 50 })
  light("stage", "sun", { kind = "directional", direction = { 0, -1, -1 }, color = "#ff8000",
                          intensity = 2 })
  light("stage", "fill", { kind = "ambient", intensity = 0.3 })
  light("stage", "bulb", { kind = "point", position = { 1, 2, 3 } })
  model("hero", "models/cube.gltf", { scene = "stage", position = { 1, 2, 3 },
                                       rotation = { 0, 45, 0 }, scale = 2, shader = "toon" })
  narrate("built")
end)
)";

TEST_CASE("script: scene3d, model, light and camera build a scene on the 3d layer") {
  Stage stage{std::string{kBuilt}};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  Scene3dDisplayable* scene = stage.stage();
  REQUIRE(scene != nullptr);
  CHECK(stage.scene.layer_of("stage") == Layer::k3d);

  const auto& camera = scene->camera().uniforms;
  CHECK(camera.at(0) == 0);
  CHECK(camera.at(1) == 1);
  CHECK(camera.at(2) == 6);
  CHECK(camera.at(4) == 1);  // look_at.y
  CHECK(camera.at(6) == kFov);

  REQUIRE(scene->models().size() == 1);
  const vnmaki::Model3d& hero = scene->models().front();
  CHECK(hero.tag == "hero");
  CHECK(hero.path == "models/cube.gltf");
  CHECK(hero.style == vnmaki::ShadingStyle::kToon);
  CHECK(hero.properties.uniforms.at(0) == 1);
  CHECK(hero.properties.uniforms.at(2) == 3);
  CHECK(hero.properties.uniforms.at(4) == 45);
  CHECK(hero.properties.uniforms.at(6) == 2);

  REQUIRE(scene->lights().size() == 3);
  const vnmaki::Light3d& sun = scene->lights().at(0);
  CHECK(sun.kind == vnmaki::LightKind::kDirectional);
  CHECK(sun.properties.uniforms.at(1) == -1);
  CHECK(sun.properties.uniforms.at(3) == doctest::Approx(1.0F));
  CHECK(sun.properties.uniforms.at(4) == doctest::Approx(kGreenOfOrange));
  CHECK(sun.properties.uniforms.at(5) == 0);
  CHECK(sun.properties.uniforms.at(6) == 2);
  CHECK(scene->lights().at(1).kind == vnmaki::LightKind::kAmbient);
  CHECK(scene->lights().at(2).kind == vnmaki::LightKind::kPoint);
}

TEST_CASE("script: a model, a light and the camera keep what a later call does not set") {
  Stage stage{R"(
label("start", function()
  scene3d("stage")
  model("hero", "models/cube.gltf", { scene = "stage", position = { 1, 2, 3 }, scale = 2 })
  model("hero", "models/blue.gltf", { scene = "stage", position = { 5, 5, 5 } })
  light("stage", "sun", { intensity = 3 })
  light("stage", "sun", { color = "#00ff00" })
  scene3d("stage", { layer = "overlay" })
  narrate("again")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  Scene3dDisplayable* scene = stage.stage();
  REQUIRE(scene != nullptr);
  REQUIRE(scene->models().size() == 1);
  CHECK(scene->models().front().path == "models/blue.gltf");
  CHECK(scene->models().front().properties.uniforms.at(0) == 5);
  CHECK(scene->models().front().properties.uniforms.at(6) == 2);  // the scale stays
  REQUIRE(scene->lights().size() == 1);
  CHECK(scene->lights().front().properties.uniforms.at(6) == 3);  // the intensity stays
  CHECK(scene->lights().front().properties.uniforms.at(3) == 0);  // the colour changed
  CHECK(scene->lights().front().properties.uniforms.at(4) == 1);
  CHECK(stage.scene.layer_of("stage") == Layer::kOverlay);
  CHECK(stage.scene.tags(Layer::k3d).empty());
  // The camera starts at (0, 0, 5) looking at the origin.
  CHECK(scene->camera().uniforms.at(2) == 5);
  CHECK(scene->camera().uniforms.at(6) == 60);
}

TEST_CASE("script: hide3d removes an object and hide removes the scene") {
  Stage stage{R"(
label("start", function()
  scene3d("stage")
  model("hero", "models/cube.gltf", { scene = "stage" })
  light("stage", "sun", {})
  hide3d("stage", "sun")
  tween("@3d.stage.hero", { y = 3, duration = 1 })
  narrate("one")
  hide("stage")
  narrate("two")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  REQUIRE(stage.stage() != nullptr);
  CHECK(stage.stage()->lights().empty());
  CHECK(stage.scene.tweening("@3d.stage.hero"));
  stage.runtime.advance();
  CHECK(stage.stage() == nullptr);
  CHECK_FALSE(stage.scene.tweening("@3d.stage.hero"));  // the tween went with the scene
  CHECK(stage.scene.timeline().empty());
}

TEST_CASE("script: the calls report their mistakes at the script line") {
  const auto has = [](const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
  };
  CHECK(has(line_error(R"(model("hero", "models/cube.gltf", { scene = "nope" }))"),
            "no 3D scene 'nope'"));
  CHECK(has(line_error(R"(model("hero", "models/cube.gltf", {}))"), "'scene'"));
  CHECK(has(line_error(R"(model("hero", "models/absent.gltf", { scene = "stage" }))"), "missing"));
  CHECK(has(line_error(R"(model("hero", "models/bad/lines.gltf", { scene = "stage" }))"),
            "triangle"));
  CHECK(
      has(line_error(R"(model("hero", "models/cube.gltf", { scene = "stage", shader = "ghost" }))"),
          "is not declared"));
  CHECK(has(line_error(R"(model("hero", "models/cube.gltf", { scene = "stage",
                                    uniforms = { amount = 1 } }))"),
            "need a shader"));
  CHECK(
      has(line_error(R"(model("a.b", "models/cube.gltf", { scene = "stage" }))"), "is not a name"));
  CHECK(has(line_error(R"(model("camera", "models/cube.gltf", { scene = "stage" }))"),
            "is not a name"));
  CHECK(has(line_error(R"(model("hero", "models/cube.gltf", { scene = "stage",
                                    position = { 1, 2 } }))"),
            "{ x, y, z }"));
  CHECK(has(line_error(R"(model("hero", "models/cube.gltf", { scene = "stage",
                                    position = { 1, 2, 1e9 } }))"),
            "position"));
  CHECK(has(line_error(R"(light("stage", "sun", { kind = "spot" }))"), "unknown light kind"));
  CHECK(
      has(line_error(R"(light("stage", "sun", { kind = "directional", position = { 0, 0, 0 } }))"),
          "only a point light"));
  CHECK(has(line_error(R"(light("stage", "sun", { kind = "point", direction = { 0, 0, 0 } }))"),
            "only a directional light"));
  CHECK(has(line_error(R"(for i = 1, 5 do light("stage", "l" .. i, { kind = "point" }) end)"),
            "at most 4 lights"));
  CHECK(has(line_error(R"(camera("stage", { fov = 500 }))"), "fov"));
  CHECK(has(line_error(R"(camera("stage", { position = "up" }))"), "{ x, y, z }"));
  CHECK(has(line_error(R"(scene3d("other", { layer = "nowhere" }))"), "unknown layer"));
  CHECK(has(line_error(R"(scene3d("eileen"))"), "already used"));
  CHECK(has(line_error(R"(show("@3d.x", { image = "eileen" }))"), "reserved for 3D objects"));
  CHECK(has(line_error(R"(hide3d("stage", "nobody"))"), "no model or light 'nobody'"));
  CHECK(has(line_error(R"(light("stage", "x", { color = "red" }))"), "#rrggbb"));
}

TEST_CASE("script: camera { x } is still the 2D camera") {
  Stage stage{R"(
label("start", function()
  scene3d("stage")
  camera { x = 0.5 }
  narrate("both")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  CHECK(stage.scene.camera().pose.position.x == 32);
  CHECK(stage.stage()->camera().uniforms.at(0) == 0);
}

// --- Tweens and waits ------------------------------------------------------------------------

constexpr std::string_view kTweened = R"(
label("start", function()
  scene3d("stage")
  model("hero", "models/cube.gltf", { scene = "stage", position = { 0, 0, 0 } })
  light("stage", "sun", { intensity = 1 })
  tween("@3d.stage.hero", { y = 2, ry = 90, scale = 3, duration = 1 })
  tween("@3d.stage.camera", { z = 9, fov = 30, duration = 1 })
  tween("@3d.stage.sun", { intensity = 0, r = 0, duration = 1 })
  wait("@3d.stage.hero")
  narrate("done")
end)
)";

TEST_CASE("script: tween and wait move a model, a light and the camera") {
  Stage stage{std::string{kTweened}};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  CHECK(stage.scene.tweening("@3d.stage.hero"));
  CHECK(stage.scene.tweening("@3d.stage.camera"));
  CHECK_FALSE(stage.scene.tweening("@3d.stage.nobody"));
  Scene3dDisplayable* scene = stage.stage();
  WaitPresenter waits{stage.runtime, stage.scene};

  stage.scene.update(kHalfSecond);
  CHECK_FALSE(waits.update());
  const auto& hero = scene->models().front().properties.uniforms;
  CHECK(hero.at(kChannelY) == doctest::Approx(kHalfY));                            // y: 0 to 2
  CHECK(hero.at(kChannelRy) == doctest::Approx(kHalfTurn));                        // ry: 0 to 90
  CHECK(hero.at(kChannelScale) == doctest::Approx(kHalfScale));                    // scale: 1 to 3
  CHECK(scene->camera().uniforms.at(kChannelZ) == doctest::Approx(kHalfCameraZ));  // z: 5 to 9
  CHECK(scene->camera().uniforms.at(kChannelFov) == doctest::Approx(kHalfFov));    // fov: 60 to 30
  CHECK(scene->lights().front().properties.uniforms.at(kChannelIntensity) ==
        doctest::Approx(kHalfIntensity));

  stage.scene.update(kHalfSecond);
  CHECK(waits.update());
  CHECK(stage.text() == "done");
  CHECK(hero.at(1) == 2);
  CHECK(scene->lights().front().properties.uniforms.at(3) == 0);
}

TEST_CASE("script: a tween of a 3D object checks its tag and its options") {
  const auto has = [](const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
  };
  const std::string model_line = R"(model("hero", "models/cube.gltf", { scene = "stage" }))";
  CHECK(has(line_error(model_line + R"(
    tween("@3d.stage.hero", { alpha = 0.5 }))"),
            "only its own channels"));
  CHECK(has(line_error(model_line + R"(
    tween("@3d.stage.hero", { rotation = 5 }))"),
            "only its own channels"));
  CHECK(has(line_error(model_line + R"(
    tween("@3d.stage.hero", { uniforms = { amount = 1 } }))"),
            "no channel 'amount'"));
  CHECK(has(line_error(R"(tween("@3d.stage.nobody", { x = 1 }))"), "nobody"));
  CHECK(has(line_error(R"(tween("@3d.none.camera", { x = 1 }))"), "none"));
  CHECK(has(line_error(model_line + R"(
    tween("@3d.stage.hero", { ry = "left" }))"),
            "bad option 'ry'"));
}

TEST_CASE("script: a click ends the waited 3D tween at its end values") {
  Stage stage{std::string{kTweened}};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  WaitPresenter waits{stage.runtime, stage.scene};
  stage.scene.update(kShortWhile);
  CHECK(waits.skip());
  CHECK(stage.text() == "done");
  CHECK(stage.stage()->models().front().properties.uniforms.at(1) == 2);
  CHECK(stage.scene.tweening("@3d.stage.camera"));  // only the waited tag ends
}

TEST_CASE("script: the uniforms of a model's game shader are tweened after its own channels") {
  if (!vnmaki::shader_compiler_available()) {
    MESSAGE("no run-time shader compiler in this build");
    return;
  }
  Stage stage{R"(
label("start", function()
  scene3d("stage")
  shader("whiten", { "amount" })
  model("hero", "models/cube.gltf", { scene = "stage", shader = "whiten",
                                       uniforms = { amount = 0.25 } })
  tween("@3d.stage.hero", { uniforms = { amount = 1 }, duration = 1 })
  wait("@3d.stage.hero")
  narrate("done")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  const vnmaki::Model3d& hero = stage.stage()->models().front();
  CHECK(hero.properties.shader != 0);
  CHECK(hero.properties.uniforms.at(vnmaki::kPoseChannels) == kStartAmount);
  stage.scene.update(kHalfSecond);
  CHECK(hero.properties.uniforms.at(vnmaki::kPoseChannels) == doctest::Approx(kHalfwayAmount));
  stage.scene.update(kHalfSecond);
  CHECK(hero.properties.uniforms.at(vnmaki::kPoseChannels) == 1.0F);
}

// --- The snapshot and the replay -------------------------------------------------------------

constexpr std::string_view kSnapshotted = R"(
label("start", function()
  layered("girl", { order = { "base" }, base = "eileen" })
  show("below", { image = "eileen", layer = "3d" })
  scene3d("stage")
  show("girl", { layer = "3d" })
  show("above", { image = "eileen", layer = "3d" })
  camera("stage", { position = { 1, 2, 3 }, look_at = { 4, 5, 6 }, fov = 40 })
  light("stage", "sun", { kind = "directional", direction = { 0, -1, 0 }, intensity = 2 })
  model("hero", "models/cube.gltf", { scene = "stage", shader = "pbr", position = { 7, 8, 9 } })
  tween("@3d.stage.hero", { ry = 90, duration = 1 })
  tween("@3d.stage.camera", { x = 10, duration = 1, loop = true })
  narrate("built")
end)
)";

TEST_CASE("snapshot: the 3D scenes are a part of their own, restored after the 2D parts") {
  Stage stage{std::string{kSnapshotted}};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  CHECK(stage.scene.tags(Layer::k3d) ==
        std::vector<std::string>{"below", "stage", "girl", "above"});
  const std::vector<std::byte> bytes = stage.scene.capture();

  // A scene without the layered definition cannot restore the sprite: use the same scene.
  stage.scene.restore({});
  CHECK(stage.scene.tags(Layer::k3d).empty());
  stage.scene.restore(bytes);
  CHECK(stage.scene.tags(Layer::k3d) ==
        std::vector<std::string>{"below", "stage", "girl", "above"});
  Scene3dDisplayable* scene = stage.stage();
  REQUIRE(scene != nullptr);
  CHECK(scene->camera().uniforms.at(0) == 1);  // a loop is at the value where it started
  CHECK(scene->camera().uniforms.at(6) == 40);
  REQUIRE(scene->models().size() == 1);
  CHECK(scene->models().front().style == vnmaki::ShadingStyle::kPbr);
  CHECK(scene->models().front().properties.uniforms.at(4) ==
        90);  // a tween that ends is at its end
  CHECK(scene->models().front().properties.uniforms.at(1) == 8);
  REQUIRE(scene->lights().size() == 1);
  CHECK(scene->lights().front().properties.uniforms.at(6) == 2);
  CHECK(stage.scene.capture() == bytes);
  // The loop starts again at time 0; the tween that ended does not.
  CHECK(stage.scene.timeline().tags() == std::vector<std::string>{"@3d.stage.camera"});
  stage.scene.update(kHalfSecond);
  CHECK(scene->camera().uniforms.at(0) == doctest::Approx(kLoopHalfway));  // from 1 to 10
}

TEST_CASE("snapshot: a game without 3D keeps the snapshot it had") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({{"images/eileen.png", ""}});
  Scene plain{vfs, kVirtualSize};
  Scene other{vfs, kVirtualSize};
  CHECK(plain.capture() == other.capture());
  CHECK(plain.capture().size() < 150);  // no "scene3d" part, as before
  const std::vector<std::byte> empty = plain.capture();
  Stage stage{std::string{kSnapshotted}};
  stage.scene.restore(empty);
  CHECK(stage.stage() == nullptr);
  CHECK(stage.scene.capture() == empty);
}

TEST_CASE("snapshot: a refused snapshot changes nothing") {
  Stage stage{std::string{kSnapshotted}};
  std::vector<std::byte> bytes = stage.scene.capture();
  const std::vector<std::byte> good = bytes;
  // Cut the snapshot short, in every way: it throws or restores, never crashes.
  for (std::size_t cut = 0; cut < bytes.size(); cut += kCutStep) {
    Scene other{stage.vfs, kVirtualSize};
    try {
      other.restore(std::span{bytes}.first(cut));
    } catch (const std::invalid_argument&) {
      CHECK(other.tags(Layer::k3d).empty());
    }
  }
  CHECK(stage.scene.capture() == good);
}

TEST_CASE("replay: a replay puts the 3D scene where the story left it") {
  Stage live{R"(
label("start", function()
  scene3d("stage")
  model("hero", "models/cube.gltf", { scene = "stage" })
  tween("@3d.stage.hero", { x = 4, duration = 1 })
  wait("@3d.stage.hero")
  narrate("one")
  narrate("two")
end)
)"};
  WaitPresenter waits{live.runtime, live.scene};
  live.scene.update(kSecond);
  REQUIRE(waits.update());
  REQUIRE(live.text() == "one");
  REQUIRE(live.runtime.label_snapshot() != nullptr);
  const vnmaki::LabelSnapshot snapshot = *live.runtime.label_snapshot();
  const std::vector<vnmaki::InputResult> inputs{live.runtime.input_log().begin(),
                                                live.runtime.input_log().end()};
  const std::vector<std::byte> expected = live.scene.capture();

  Stage fresh{R"(
label("start", function()
  scene3d("stage")
  model("hero", "models/cube.gltf", { scene = "stage" })
  tween("@3d.stage.hero", { x = 4, duration = 1 })
  wait("@3d.stage.hero")
  narrate("one")
  narrate("two")
end)
)"};
  fresh.runtime.replay(snapshot, inputs, 2);  // the line after the wait
  REQUIRE_MESSAGE(fresh.runtime.state() == vnmaki::RunState::kWaiting, fresh.runtime.error());
  CHECK(fresh.text() == "one");
  REQUIRE(fresh.stage() != nullptr);
  CHECK(fresh.stage()->models().front().properties.uniforms.at(0) == 4);
  CHECK_FALSE(fresh.scene.tweening("@3d.stage.hero"));
  CHECK(fresh.scene.capture() == expected);
}

}  // namespace
