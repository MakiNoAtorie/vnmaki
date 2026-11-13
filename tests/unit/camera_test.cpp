// The camera (docs/SPEC.md section 10): the view maths, the shake noise, the script calls
// `camera`, `shake` and `layer(name, { parallax })`, tweens and waits on "@camera", the snapshot,
// replay and reduced motion. Time is a fake clock. The frames are checked on a GPU in
// tests/render/camera_render_test.cpp.

#include "engine/scene/camera.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/anim/wait_presenter.hpp"
#include "engine/core/byte_stream.hpp"
#include "engine/runtime/headless_runner.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"
#include "tests/support/recording_renderer.hpp"

namespace {

using vnmaki::CameraState;
using vnmaki::CameraView;
using vnmaki::PointF;
using vnmaki::Scene;
using vnmaki::Shake;
using vnmaki::WaitPresenter;

constexpr vnmaki::Size kVirtualSize{.width = 32, .height = 32};
constexpr vnmaki::Size kSpriteSize{.width = 8, .height = 16};
constexpr vnmaki::TextureId kSpriteTexture{.value = 1};

constexpr float kCenter = 16;
constexpr float kPanX = 10;
constexpr float kPanY = 6;
constexpr float kHalfFactor = 0.5F;
constexpr float kZoomTwice = 2;
constexpr float kQuarterTurn = 90;
constexpr float kTolerance = 1e-4F;
constexpr double kHalfSecond = 0.5;
constexpr double kSecond = 1.0;
constexpr double kTwoSeconds = 2.0;
constexpr double kSeventh = 1.0 / 7.0;
constexpr double kSixtieth = 1.0 / 60.0;
constexpr float kStrengthPixels = 8;  // 0.25 of the virtual height
constexpr std::uint32_t kFormatVersion = 2;  // of Scene::capture()

// --- The view --------------------------------------------------------------------------------

CameraState pan_zoom_rotation(PointF pan, float zoom, float rotation) {
  CameraState camera;
  camera.pose.position = pan;
  camera.pose.scale = {.x = zoom, .y = zoom};
  camera.pose.rotation = rotation;
  return camera;
}

void check_near(PointF actual, PointF expected) {
  CHECK(actual.x == doctest::Approx(expected.x).epsilon(kTolerance));
  CHECK(actual.y == doctest::Approx(expected.y).epsilon(kTolerance));
}

TEST_CASE("camera: the default camera changes nothing") {
  const CameraState camera;
  CHECK(camera.is_default());
  const CameraView view{camera, {}, kVirtualSize, 0};
  CHECK(view.identity());
  CHECK(view.apply({.x = 3, .y = 5}) == PointF{.x = 3, .y = 5});
}

TEST_CASE("camera: a pan moves the content the other way, by the parallax of the layer") {
  CameraState camera = pan_zoom_rotation({.x = kPanX, .y = kPanY}, 1, 0);
  camera.parallax.at(1) = kHalfFactor;
  const PointF middle{.x = kCenter, .y = kCenter};
  check_near(CameraView{camera, {}, kVirtualSize, 0}.apply(middle),
             {.x = kCenter - kPanX, .y = kCenter - kPanY});
  check_near(CameraView{camera, {}, kVirtualSize, 1}.apply(middle),
             {.x = kCenter - kPanX * kHalfFactor, .y = kCenter - kPanY * kHalfFactor});
  // The shake adds to the pan, so it also follows the parallax.
  check_near(CameraView{camera, {.x = 2, .y = 0}, kVirtualSize, 1}.apply(middle),
             {.x = kCenter - (kPanX + 2) * kHalfFactor, .y = kCenter - kPanY * kHalfFactor});
}

TEST_CASE("camera: zoom and rotation turn about the centre of the screen") {
  const PointF right{.x = kCenter + 4, .y = kCenter};
  check_near(CameraView{pan_zoom_rotation({}, kZoomTwice, 0), {}, kVirtualSize, 0}.apply(right),
             {.x = kCenter + 8, .y = kCenter});
  // Clockwise on screen, y down: the right of the centre goes below it.
  check_near(CameraView{pan_zoom_rotation({}, 1, kQuarterTurn), {}, kVirtualSize, 0}.apply(right),
             {.x = kCenter, .y = kCenter + 4});
  // A zoom of 0 or less is drawn as the smallest zoom.
  const CameraView flat{pan_zoom_rotation({}, 0, 0), {}, kVirtualSize, 0};
  CHECK(flat.apply(right).x == doctest::Approx(kCenter + 4 * vnmaki::kMinZoom));
}

// --- The shake -------------------------------------------------------------------------------

TEST_CASE("camera: the shake is a fixed function of time that dies away") {
  const Shake shake{.strength = kStrengthPixels, .duration = kSecond, .elapsed = 0};
  float largest = 0;
  PointF last{};
  bool moved = false;
  for (int i = 0; i < 40; ++i) {
    Shake now = shake;
    now.elapsed = static_cast<double>(i) * kSixtieth;
    const PointF offset = vnmaki::shake_offset(now);
    CHECK(vnmaki::shake_offset(now) == offset);  // the same at the same time
    const auto bound = static_cast<float>(kStrengthPixels * (1 - now.elapsed / now.duration));
    CHECK(std::abs(offset.x) <= bound + kTolerance);
    CHECK(std::abs(offset.y) <= bound + kTolerance);
    largest = std::max({largest, std::abs(offset.x), std::abs(offset.y)});
    moved = moved || offset != last;
    last = offset;
  }
  CHECK(moved);
  CHECK(largest > 0);
  // A shake that has run its time, or has none, is no offset.
  CHECK(vnmaki::shake_offset({.strength = kStrengthPixels, .duration = kSecond, .elapsed = 2}) ==
        PointF{});
  CHECK(vnmaki::shake_offset({.strength = kStrengthPixels, .duration = 0, .elapsed = 0}) ==
        PointF{});
}

TEST_CASE("camera: the shake noise is the one the GPU golden generator repeats") {
  // tools/generators/camera_fixtures.py: shake_offset(8, 1, 0.3).
  constexpr float kExpectedX = 3.3126342F;
  constexpr float kExpectedY = 2.6233634F;
  constexpr double kAt = 0.3;
  const PointF offset =
      vnmaki::shake_offset({.strength = kStrengthPixels, .duration = kSecond, .elapsed = kAt});
  check_near(offset, {.x = kExpectedX, .y = kExpectedY});
}

// --- The script ------------------------------------------------------------------------------

vnmaki::Vfs game_files(const std::string& script) {
  return vnmaki::test::memory_vfs({
      {"scripts/main.lua", script},
      {"images/eileen.png", ""},
      {"images/bg/room.png", ""},
  });
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
  [[nodiscard]] bool at_camera_wait() const {
    const vnmaki::BlockingRequest* request = runtime.request();
    const auto* wait = request != nullptr ? std::get_if<vnmaki::WaitRequest>(request) : nullptr;
    return wait != nullptr && wait->tag == vnmaki::kCameraTag;
  }
  [[nodiscard]] const vnmaki::DisplayProperties& pose() const { return scene.camera().pose; }

  vnmaki::Vfs vfs;
  Scene scene;
  vnmaki::ScriptRuntime runtime;
};

// The error of the story whose `start` label runs `lines` (from line 3).
std::string line_error(const std::string& lines) {
  const Stage stage{"label(\"start\", function()\n  show(\"eileen\")\n" + lines + "\nend)\n"};
  REQUIRE(stage.runtime.state() == vnmaki::RunState::kFailed);
  return stage.runtime.error();
}

constexpr std::string_view kMove = R"(
label("start", function()
  show("eileen")
  camera { x = 0.125, y = 0.25, zoom = 2, rotation = 40, duration = 1 }
  wait("@camera")
  narrate("moved")
  narrate("done")
end)
)";

TEST_CASE("script: camera tweens the view and wait('@camera') waits for it") {
  Stage stage{std::string{kMove}};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  REQUIRE(stage.at_camera_wait());
  CHECK(stage.scene.tweening(vnmaki::kCameraTag));
  CHECK(stage.pose().position == PointF{.x = 0, .y = 0});

  WaitPresenter waits{stage.runtime, stage.scene};
  stage.scene.update(kHalfSecond);
  CHECK_FALSE(waits.update());
  CHECK(stage.pose().position.x == doctest::Approx(2));  // x is a fraction of the width: 4 / 2
  CHECK(stage.pose().position.y == doctest::Approx(4));
  CHECK(stage.pose().scale.x == doctest::Approx(1.5));
  CHECK(stage.pose().rotation == doctest::Approx(20));
  stage.scene.update(kHalfSecond);
  CHECK_FALSE(stage.scene.tweening(vnmaki::kCameraTag));
  CHECK(waits.update());
  CHECK(stage.text() == "moved");
  CHECK(stage.pose().position == PointF{.x = 4, .y = 8});
  CHECK(stage.pose().scale.x == 2);
  CHECK(stage.pose().rotation == 40);
}

TEST_CASE("script: the camera moves the same with any step size") {
  for (const double step : {kSixtieth, kSeventh}) {
    CAPTURE(step);
    Stage stage{std::string{kMove}};
    double elapsed = 0;
    while (stage.scene.tweening(vnmaki::kCameraTag)) {
      stage.scene.update(step);
      elapsed += step;
      REQUIRE(elapsed < kTwoSeconds);
    }
    CHECK(stage.pose().position == PointF{.x = 4, .y = 8});
    CHECK(stage.pose().scale.x == 2);
    CHECK(stage.pose().rotation == 40);
  }
}

TEST_CASE("script: a click puts the camera at its end value") {
  Stage stage{std::string{kMove}};
  WaitPresenter waits{stage.runtime, stage.scene};
  stage.scene.update(kHalfSecond);
  CHECK(waits.skip());
  CHECK(stage.pose().position == PointF{.x = 4, .y = 8});
  CHECK(stage.text() == "moved");
}

TEST_CASE("script: a camera without a duration moves at once and does not block") {
  Stage stage{R"(
label("start", function()
  camera { x = 0.25 }
  narrate("here")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  CHECK(stage.pose().position.x == 8);
  CHECK_FALSE(stage.scene.tweening(vnmaki::kCameraTag));
  CHECK(stage.text() == "here");
}

TEST_CASE("script: tween('@camera') takes zoom or scale, and eases like any tween") {
  Stage stage{R"(
label("start", function()
  tween("@camera", { zoom = 3, rotation = -10, duration = 1, ease = "in_quad" })
  wait("@camera")
  tween("@camera", { scale = 2 })
  narrate("done")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  stage.scene.update(kHalfSecond);  // in_quad at 0.5 is 0.25
  CHECK(stage.pose().scale.x == doctest::Approx(1.5));
  CHECK(stage.pose().rotation == doctest::Approx(-2.5));
  stage.scene.update(kHalfSecond);
  WaitPresenter waits{stage.runtime, stage.scene};
  REQUIRE(waits.update());
  REQUIRE(stage.text() == "done");
  stage.scene.update(kSecond);  // the second tween goes on from 3 to 2
  CHECK(stage.pose().scale.x == 2);
}

TEST_CASE("script: the camera is a channel of the timeline, so groups and loops work") {
  Stage stage{R"(
label("start", function()
  show("eileen")
  parallel {
    { "@camera", x = 0.25, duration = 1 },
    { "eileen", x = 0.75, duration = 1 },
  }
  wait_all()
  narrate("done")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  stage.scene.update(kSecond);
  CHECK(stage.pose().position.x == 8);
  CHECK(stage.scene.find("eileen")->properties().position.x == 24);
}

// --- The snapshot and the replay -------------------------------------------------------------

TEST_CASE("script: the camera and the parallax are in the snapshot") {
  Stage stage{R"(
label("start", function()
  layer("background", { parallax = 0.5 })
  camera { x = 0.125, zoom = 1.5, rotation = 5 }
  narrate("set")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  const std::vector<std::byte> bytes = stage.scene.capture();

  const vnmaki::Vfs vfs = game_files("");
  Scene other{vfs, kVirtualSize};
  other.restore(bytes);
  CHECK(other.camera().pose.position == PointF{.x = 4, .y = 0});
  CHECK(other.camera().pose.scale == PointF{.x = 1.5F, .y = 1.5F});
  CHECK(other.camera().pose.rotation == 5);
  CHECK(other.camera().parallax.at(0) == kHalfFactor);
  CHECK(other.camera().parallax.at(1) == 1);
  CHECK(other.capture() == bytes);

  // A restore puts the default camera back when the snapshot has none, and a default camera
  // leaves the snapshot as it was before the camera existed.
  Scene fresh{vfs, kVirtualSize};
  const std::vector<std::byte> empty = fresh.capture();
  other.restore(empty);
  CHECK(other.camera().is_default());
  CHECK(other.capture() == empty);
  const Stage reset{R"(
label("start", function()
  camera { x = 0.5, rotation = 30 }
  camera { x = 0, rotation = 0 }
  narrate("home")
end)
)"};
  CHECK(reset.scene.camera().is_default());
  CHECK(reset.scene.capture() == empty);
}

// A snapshot with only a "camera" part: the pan, the zoom, the rotation and no parallax.
std::vector<std::byte> camera_snapshot(float zoom) {
  vnmaki::ByteWriter part;
  part.f32(0);
  part.f32(0);
  part.f32(zoom);
  part.f32(0);
  part.u32(0);
  vnmaki::ByteWriter whole;
  whole.u32(kFormatVersion);
  whole.u32(1);
  whole.string("camera");
  whole.block(part.take());
  return whole.take();
}

TEST_CASE("script: a snapshot with a camera that cannot be is refused") {
  const vnmaki::Vfs vfs = game_files("");
  Scene scene{vfs, kVirtualSize};
  CHECK_NOTHROW(scene.restore(camera_snapshot(kZoomTwice)));
  CHECK(scene.camera().pose.scale.x == kZoomTwice);
  CHECK_THROWS_AS(scene.restore(camera_snapshot(0)), std::invalid_argument);
  CHECK_THROWS_AS(scene.restore(camera_snapshot(-1)), std::invalid_argument);
  CHECK(scene.camera().pose.scale.x == kZoomTwice);  // a refused snapshot changes nothing
}

TEST_CASE("script: a replay puts the camera at its end value and starts no shake") {
  Stage live{R"(
label("start", function()
  camera { x = 0.125, duration = 1 }
  shake { strength = 0.1, duration = 1 }
  wait("@camera")
  narrate("one")
  narrate("two")
end)
)"};
  REQUIRE(live.at_camera_wait());
  REQUIRE(live.scene.active_shake() != nullptr);
  live.scene.update(kTwoSeconds);
  live.runtime.advance();
  REQUIRE(live.text() == "one");
  REQUIRE(live.runtime.label_snapshot() != nullptr);
  const vnmaki::LabelSnapshot snapshot = *live.runtime.label_snapshot();
  const std::vector<vnmaki::InputResult> inputs{live.runtime.input_log().begin(),
                                                live.runtime.input_log().end()};

  Stage fresh{R"(
label("start", function()
  camera { x = 0.125, duration = 1 }
  shake { strength = 0.1, duration = 1 }
  wait("@camera")
  narrate("one")
  narrate("two")
end)
)"};
  fresh.runtime.replay(snapshot, inputs, 2);  // the line after the wait
  REQUIRE_MESSAGE(fresh.runtime.state() == vnmaki::RunState::kWaiting, fresh.runtime.error());
  CHECK(fresh.text() == "one");
  CHECK(fresh.pose().position.x == 4);
  CHECK_FALSE(fresh.scene.tweening(vnmaki::kCameraTag));
  CHECK(fresh.scene.active_shake() == nullptr);
  CHECK(fresh.scene.capture() == live.scene.capture());
}

TEST_CASE("script: a looping camera tween is in the snapshot and starts again") {
  Stage stage{R"(
label("start", function()
  tween("@camera", { x = 0.25, duration = 1, loop = true, yoyo = true })
  narrate("swaying")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  CHECK_FALSE(stage.scene.tweening(vnmaki::kCameraTag));  // a loop never blocks a wait
  stage.scene.update(kHalfSecond);
  CHECK(stage.pose().position.x == doctest::Approx(4));
  const std::vector<std::byte> bytes = stage.scene.capture();

  Scene other{stage.vfs, kVirtualSize};
  other.restore(bytes);
  CHECK(other.camera().pose.position.x == 0);  // the loop restarts from where it began
  other.update(kHalfSecond);
  CHECK(other.camera().pose.position.x == doctest::Approx(4));
}

// --- The shake in the script -----------------------------------------------------------------

constexpr std::string_view kShake = R"(
label("start", function()
  shake { strength = 0.25, duration = 1 }
  wait("@camera")
  narrate("still")
end)
)";

TEST_CASE("script: shake runs for its duration, blocks wait('@camera') and a click ends it") {
  Stage stage{std::string{kShake}};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  REQUIRE(stage.at_camera_wait());
  REQUIRE(stage.scene.active_shake() != nullptr);
  CHECK(stage.scene.active_shake()->strength == kStrengthPixels);
  CHECK(stage.scene.active_shake()->duration == kSecond);
  CHECK(stage.scene.tweening(vnmaki::kCameraTag));
  CHECK(stage.scene.tweening(""));
  CHECK(stage.pose().position == PointF{});  // the shake is not the camera's pose

  WaitPresenter waits{stage.runtime, stage.scene};
  stage.scene.update(kHalfSecond);
  CHECK_FALSE(waits.update());
  stage.scene.update(kHalfSecond);
  CHECK(stage.scene.active_shake() == nullptr);
  CHECK(waits.update());
  CHECK(stage.text() == "still");

  Stage clicked{std::string{kShake}};
  WaitPresenter clicks{clicked.runtime, clicked.scene};
  CHECK(clicks.skip());
  CHECK(clicked.scene.active_shake() == nullptr);
  CHECK(clicked.text() == "still");
}

TEST_CASE("script: shake does not use the random numbers of the story") {
  const auto rolled = [](std::string_view shake) {
    Stage stage{"label(\"start\", function()\n" + std::string{shake} +
                "  narrate(tostring(rand(1, 1000000)))\nend)\n"};
    REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
    return stage.text();
  };
  CHECK(rolled("  shake { strength = 0.1 }\n") == rolled(""));
}

TEST_CASE("script: a shake draws the same on every run") {
  const auto offset = [] {
    Stage stage{std::string{kShake}};
    stage.scene.update(kSeventh);
    return vnmaki::shake_offset(*stage.scene.active_shake());
  };
  CHECK(offset() == offset());
}

// --- Reduced motion --------------------------------------------------------------------------

TEST_CASE("script: reduce_motion in the prefs stops the shake and makes camera moves instant") {
  Stage stage{R"(
label("start", function()
  persistent.prefs = { reduce_motion = true }
  shake { strength = 0.25, duration = 1 }
  camera { x = 0.125, zoom = 2, duration = 1 }
  wait("@camera")
  narrate("calm")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  CHECK(stage.scene.active_shake() == nullptr);
  CHECK(stage.pose().position.x == 4);
  CHECK(stage.pose().scale.x == 2);
  CHECK_FALSE(stage.scene.tweening(vnmaki::kCameraTag));
  CHECK(stage.at_camera_wait());  // the wait is still a step: a replay counts the same steps
}

TEST_CASE("script: reduce_motion also makes a tween of the camera instant, and loops stop") {
  Stage stage{R"(
label("start", function()
  persistent.prefs = { reduce_motion = true }
  tween("@camera", { x = 0.25, duration = 5, delay = 2 })
  narrate("a")
  tween("@camera", { rotation = 30, duration = 5, loop = true, yoyo = true })
  narrate("b")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  CHECK(stage.pose().position.x == 8);
  stage.runtime.advance();
  CHECK(stage.pose().rotation == 30);
  stage.scene.update(kSecond);
  CHECK(stage.pose().rotation == 30);
}

TEST_CASE("script: without prefs, or without the key, or with it off, motion is not reduced") {
  for (const std::string_view prefs : {"", "persistent.prefs = {}", "persistent.prefs = {"
                                                                    " reduce_motion = false }",
                                       "persistent.prefs = 7"}) {
    CAPTURE(prefs);
    Stage stage{"label(\"start\", function()\n" + std::string{prefs} +
                "\n  shake { strength = 0.25 }\n  camera { x = 0.125, duration = 1 }\n"
                "  narrate(\"go\")\nend)\n"};
    REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
    CHECK(stage.scene.active_shake() != nullptr);
    CHECK(stage.pose().position.x == 0);
    CHECK(stage.scene.tweening(vnmaki::kCameraTag));
  }
}

TEST_CASE("script: reduced motion still checks the arguments of a shake") {
  CHECK(line_error("persistent.prefs = { reduce_motion = true }\n  shake { strength = 5 }") ==
        "scripts/main.lua:4: option 'strength' must be a number from 0 to 1");
}

// --- Parallax and drawing --------------------------------------------------------------------

// Draws the scene to a recorder, after giving the sprites a texture.
std::vector<vnmaki::test::RecordingRenderer::Draw> drawn(Stage& stage) {
  for (const std::string_view tag : {"far", "near", "hud"}) {
    if (auto* image = dynamic_cast<vnmaki::ImageDisplayable*>(stage.scene.find(tag))) {
      image->set_texture(kSpriteTexture, kSpriteSize);
    }
  }
  vnmaki::test::RecordingRenderer renderer;
  stage.scene.draw(renderer);
  return renderer.draws;
}

constexpr std::string_view kLayers = R"(
label("start", function()
  show("far", { image = "eileen", layer = "background", x = 0.5, y = 1 })
  show("near", { image = "eileen", layer = "characters", x = 0.5, y = 1 })
  show("hud", { image = "eileen", layer = "ui", x = 0.5, y = 1 })
  layer("background", { parallax = 0.5 })
  camera { x = 0.25 }
  narrate("framed")
end)
)";

TEST_CASE("script: layers pan by their parallax, and the ui layer stays") {
  Stage stage{std::string{kLayers}};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  const auto draws = drawn(stage);
  REQUIRE(draws.size() == 3);
  // Each sprite is 8 wide, standing on the bottom edge of a screen 32 wide: x from 12 to 20. The
  // camera pans 8 pixels, so the content moves left by 8 (the far layer by half that).
  constexpr float kFarLeft = 8;   // 12 - 4
  constexpr float kNearLeft = 4;  // 12 - 8
  constexpr float kUiLeft = 12;
  CHECK(draws.at(0).quad.corners.at(0).x == doctest::Approx(kFarLeft));
  CHECK(draws.at(1).quad.corners.at(0).x == doctest::Approx(kNearLeft));
  CHECK(draws.at(2).quad.corners.at(0).x == doctest::Approx(kUiLeft));
  CHECK(draws.at(1).quad.corners.at(0).y == doctest::Approx(16));
}

TEST_CASE("script: zoom and rotation move every layer from the background to the overlay") {
  Stage stage{R"(
label("start", function()
  show("far", { image = "eileen", layer = "background", x = 0.5, y = 0.5 })
  show("near", { image = "eileen", layer = "overlay", x = 0.5, y = 0.5 })
  show("hud", { image = "eileen", layer = "transient", x = 0.5, y = 0.5 })
  camera { zoom = 2 }
  narrate("framed")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == vnmaki::RunState::kWaiting, stage.runtime.error());
  const auto draws = drawn(stage);
  REQUIRE(draws.size() == 3);
  // A sprite 8 wide at the centre (anchored at its bottom centre): left edge x = 12, zoomed about
  // the centre of the screen to 16 + (12 - 16) * 2 = 8, and twice as wide.
  CHECK(draws.at(0).quad.corners.at(0).x == doctest::Approx(8));
  CHECK(draws.at(0).quad.corners.at(1).x == doctest::Approx(24));
  CHECK(draws.at(1).quad.corners.at(0).x == doctest::Approx(8));
  CHECK(draws.at(2).quad.corners.at(0).x == doctest::Approx(12));
}

TEST_CASE("script: a shake moves the layers by its offset") {
  Stage shown{R"(
label("start", function()
  show("near", { image = "eileen", x = 0.5, y = 1 })
  shake { strength = 0.25, duration = 1 }
  narrate("shaking")
end)
)"};
  REQUIRE_MESSAGE(shown.runtime.state() == vnmaki::RunState::kWaiting, shown.runtime.error());
  shown.scene.update(kSeventh);
  const PointF offset = vnmaki::shake_offset(*shown.scene.active_shake());
  CHECK(offset != PointF{});
  const auto draws = drawn(shown);
  REQUIRE(draws.size() == 1);
  CHECK(draws.at(0).quad.corners.at(0).x == doctest::Approx(12 - offset.x));
  CHECK(draws.at(0).quad.corners.at(0).y == doctest::Approx(16 - offset.y));
}

// --- Errors and the transcript ---------------------------------------------------------------

TEST_CASE("script: bad camera calls name the script line") {
  CHECK(line_error("camera(1)") ==
        "scripts/main.lua:3: bad argument #1 to 'camera' (table expected, got number)");
  CHECK(line_error("camera { zoom = 'big' }") ==
        "scripts/main.lua:3: bad option 'zoom' to 'camera' (number expected, got string)");
  CHECK(line_error("camera { zoom = 0 }") ==
        "scripts/main.lua:3: option 'zoom' must be a number above 0");
  CHECK(line_error("camera { ease = 'wobble' }").starts_with("scripts/main.lua:3: unknown easing"));
  CHECK(line_error("camera { alpha = 0.5 }") ==
        "scripts/main.lua:3: the camera has only x, y, zoom and rotation to tween");
  CHECK(line_error("tween('@camera', { uniforms = { amount = 1 } })") ==
        "scripts/main.lua:3: '@camera' has no uniform 'amount' (set a shader that declares it)");
  CHECK(line_error("show('@camera', { image = 'eileen' })") ==
        "scripts/main.lua:3: the tag '@camera' is reserved for the camera");
  CHECK(line_error("shake { duration = 1 }") == "scripts/main.lua:3: shake() needs a strength");
  CHECK(line_error("shake { strength = 2 }") ==
        "scripts/main.lua:3: option 'strength' must be a number from 0 to 1");
  CHECK(line_error("shake { strength = 0.1, duration = 0 }") ==
        "scripts/main.lua:3: option 'duration' must be a number above 0 and up to 86400");
  CHECK(line_error("shake { strength = 'hard' }") ==
        "scripts/main.lua:3: bad option 'strength' to 'shake' (number expected, got string)");
  CHECK(line_error("shake()") ==
        "scripts/main.lua:3: bad argument #1 to 'shake' (table expected, got nil)");
}

TEST_CASE("script: bad parallax calls name the script line") {
  CHECK(line_error("layer('ui', { parallax = 0.5 })") ==
        "scripts/main.lua:3: the camera does not move the layer 'ui'");
  CHECK(line_error("layer('transient', { parallax = 0.5 })") ==
        "scripts/main.lua:3: the camera does not move the layer 'transient'");
  CHECK(line_error("layer('sky', { parallax = 0.5 })")
            .starts_with("scripts/main.lua:3: unknown layer 'sky'"));
  CHECK(line_error("layer('background', { parallax = 'deep' })") ==
        "scripts/main.lua:3: bad option 'parallax' to 'layer' (number expected, got string)");
  CHECK(line_error("layer('background', { parallax = 1000 })") ==
        "scripts/main.lua:3: option 'parallax' must be a number from -100 to 100");
}

TEST_CASE("script: the headless runner ends a camera wait at once") {
  const vnmaki::Vfs vfs = game_files(std::string{kMove});
  Scene scene{vfs, kVirtualSize};
  vnmaki::HeadlessOptions options;
  options.runtime.scene = &scene;
  options.runtime.scene_state = &scene;
  const std::vector<vnmaki::ScriptInput> inputs{vnmaki::AdvanceInput{}, vnmaki::AdvanceInput{}};
  const vnmaki::HeadlessResult result = vnmaki::run_headless(vfs, inputs, options);
  CHECK(result.outcome == vnmaki::HeadlessOutcome::kFinished);
  CHECK(result.transcript == std::vector<std::string>{"label start", "show eileen", "camera",
                                                      "wait @camera", "narrate moved",
                                                      "narrate done"});
}

TEST_CASE("script: without a scene the camera calls are checked and do nothing") {
  const vnmaki::Vfs vfs = game_files(R"(
label("start", function()
  camera { x = 0.5 }
  shake { strength = 0.1 }
  layer("background", { parallax = 0.5 })
  wait("@camera")
  narrate("ok")
end)
)");
  const std::vector<vnmaki::ScriptInput> inputs{vnmaki::AdvanceInput{}};
  const vnmaki::HeadlessResult result = vnmaki::run_headless(vfs, inputs);
  CHECK(result.outcome == vnmaki::HeadlessOutcome::kFinished);
  CHECK(result.transcript.back() == "narrate ok");
}

}  // namespace
