// The scene (docs/SPEC.md sections 6, 7 and 9), without a GPU: scripts run through ScriptRuntime
// and the tests read what is shown through Scene's public interface. Expected positions are
// worked out by hand for a 1280x720 virtual resolution.

#include "engine/scene/scene.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/runtime/headless_runner.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/displayable.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::DisplayProperties;
using vnmaki::Layer;
using vnmaki::PointF;
using vnmaki::RectF;
using vnmaki::RunState;
using vnmaki::Scene;
using Lines = std::vector<std::string>;
using Bytes = std::vector<std::byte>;

constexpr vnmaki::Size kVirtualSize{.width = 1280, .height = 720};

// A game with one script and the image files the tests show. The scene only checks that an
// image file exists, so the files are empty.
vnmaki::Vfs game_files(const std::string& script) {
  return vnmaki::test::memory_vfs({
      {"scripts/main.lua", script},
      {"images/bg/classroom.png", ""},
      {"images/bg/street.jpg", ""},
      {"images/eileen.png", ""},
      {"images/eileen_happy.png", ""},
      {"images/lucy.png", ""},
      {"images/rain.png", ""},
  });
}

class CommandRecorder final : public vnmaki::ScriptObserver {
 public:
  void on_scene_command(std::string_view command) override { commands.emplace_back(command); }

  Lines commands;
};

// A script started at its `start` label, with a scene.
struct Stage {
  explicit Stage(const std::string& script)
      : vfs(game_files(script)), scene(vfs, kVirtualSize), runtime(vfs, options()) {
    runtime.load_scripts();
    runtime.start("start");
  }

  [[nodiscard]] vnmaki::ScriptRuntimeOptions options() {
    vnmaki::ScriptRuntimeOptions result;
    result.observer = &recorder;
    result.scene_state = &scene;
    result.scene = &scene;
    return result;
  }

  vnmaki::Vfs vfs;
  CommandRecorder recorder;
  Scene scene;
  vnmaki::ScriptRuntime runtime;
};

// The error of a story whose `start` label is the one line `line` (line 2 of the script).
std::string line_error(const std::string& line) {
  const Stage stage{"label(\"start\", function()\n" + line + "\nend)\n"};
  REQUIRE(stage.runtime.state() == RunState::kFailed);
  return stage.runtime.error();
}

const DisplayProperties& properties_of(const Scene& scene, std::string_view tag) {
  const vnmaki::ImageDisplayable* displayable = scene.find(tag);
  REQUIRE(displayable != nullptr);
  return displayable->properties();
}

std::string path_of(const Scene& scene, std::string_view tag) {
  const vnmaki::ImageDisplayable* displayable = scene.find(tag);
  REQUIRE(displayable != nullptr);
  return displayable->path();
}

constexpr std::string_view kStory = R"(
label("start", function()
  scene("bg/classroom")
  show("eileen", { at = "left" })
  jump("talk")
end)

label("talk", function()
  say("Eileen", "one")
  show("lucy", { at = "right" })
  local choice = menu { "stay", "go" }
  if choice == 2 then
    hide("eileen")
  end
  show("lucy", { image = "eileen_happy", x = 0.5 })
  narrate("three")
end)
)";

}  // namespace

TEST_CASE("scene: scene, show and hide change what is on the layers") {
  Stage stage{R"(
label("start", function()
  scene("bg/classroom")
  show("eileen", { at = "left" })
  show("lucy", { x = 0.5, y = 0.25 })
  say("Eileen", "hi")
  hide("eileen")
  hide("nobody")
  narrate("gone")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  const Scene& scene = stage.scene;

  CHECK(scene.tags(Layer::kBackground) == Lines{"background"});
  CHECK(path_of(scene, "background") == "images/bg/classroom.png");
  CHECK(properties_of(scene, "background").position == PointF{.x = 0, .y = 0});
  CHECK(properties_of(scene, "background").anchor == PointF{.x = 0, .y = 0});

  CHECK(scene.tags(Layer::kCharacters) == Lines{"eileen", "lucy"});
  CHECK(path_of(scene, "eileen") == "images/eileen.png");
  CHECK(properties_of(scene, "eileen").position == PointF{.x = 320, .y = 720});
  CHECK(properties_of(scene, "eileen").anchor == PointF{.x = 0.5F, .y = 1});
  CHECK(properties_of(scene, "lucy").position == PointF{.x = 640, .y = 180});

  for (const Layer layer :
       {Layer::kMaster, Layer::k3d, Layer::kOverlay, Layer::kUi, Layer::kTransient}) {
    CHECK(scene.tags(layer).empty());
  }

  stage.runtime.advance();
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  CHECK(scene.tags(Layer::kCharacters) == Lines{"lucy"});
  CHECK(scene.find("eileen") == nullptr);
  CHECK(scene.tags(Layer::kBackground) == Lines{"background"});
}

TEST_CASE("scene: named positions stand a quarter, a half and three quarters across") {
  const Stage stage{R"(
label("start", function()
  show("eileen", { at = "left" })
  show("lucy", { at = "center" })
  show("rain", { at = "right" })
  show("eileen_happy")
  narrate("shown")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());

  CHECK(properties_of(stage.scene, "eileen").position == PointF{.x = 320, .y = 720});
  CHECK(properties_of(stage.scene, "lucy").position == PointF{.x = 640, .y = 720});
  CHECK(properties_of(stage.scene, "rain").position == PointF{.x = 960, .y = 720});
  // Without a position a new tag stands at the centre.
  CHECK(properties_of(stage.scene, "eileen_happy").position == PointF{.x = 640, .y = 720});
  CHECK(properties_of(stage.scene, "eileen_happy").anchor == PointF{.x = 0.5F, .y = 1});
}

TEST_CASE("scene: show replaces a tagged displayable and keeps what the call does not set") {
  Stage stage{R"(
label("start", function()
  show("eileen", { at = "left" })
  show("lucy")
  show("eileen", { image = "eileen_happy" })
  narrate("replaced")
  show("eileen", { x = 0.75 })
  narrate("moved")
  show("eileen", { layer = "overlay" })
  narrate("lifted")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  const Scene& scene = stage.scene;

  CHECK(scene.tags(Layer::kCharacters) == Lines{"eileen", "lucy"});
  CHECK(path_of(scene, "eileen") == "images/eileen_happy.png");
  CHECK(properties_of(scene, "eileen").position == PointF{.x = 320, .y = 720});

  stage.runtime.advance();
  CHECK(path_of(scene, "eileen") == "images/eileen.png");
  CHECK(properties_of(scene, "eileen").position == PointF{.x = 960, .y = 720});

  stage.runtime.advance();
  CHECK(scene.tags(Layer::kCharacters) == Lines{"lucy"});
  CHECK(scene.tags(Layer::kOverlay) == Lines{"eileen"});
  CHECK(properties_of(scene, "eileen").position == PointF{.x = 960, .y = 720});
}

TEST_CASE("scene: scene clears the master and characters layers and sets the background") {
  Stage stage{R"(
label("start", function()
  scene("bg/classroom")
  show("eileen")
  show("lucy", { layer = "master" })
  show("rain", { layer = "overlay" })
  narrate("before")
  scene("bg/street")
  narrate("after")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  const Scene& scene = stage.scene;
  CHECK(scene.tags(Layer::kMaster) == Lines{"lucy"});
  CHECK(scene.tags(Layer::kCharacters) == Lines{"eileen"});

  stage.runtime.advance();

  CHECK(scene.tags(Layer::kBackground) == Lines{"background"});
  // No street.png exists, so the name resolves to the JPEG.
  CHECK(path_of(scene, "background") == "images/bg/street.jpg");
  CHECK(scene.tags(Layer::kMaster).empty());
  CHECK(scene.tags(Layer::kCharacters).empty());
  CHECK(scene.tags(Layer::kOverlay) == Lines{"rain"});
}

TEST_CASE("scene: the layers stack from background up to transient") {
  Lines names;
  names.reserve(vnmaki::kLayerOrder.size());
  for (const Layer layer : vnmaki::kLayerOrder) {
    names.emplace_back(vnmaki::layer_name(layer));
    CHECK(vnmaki::layer_from_name(vnmaki::layer_name(layer)) == layer);
  }
  CHECK(names == Lines{"background", "master", "characters", "3d", "overlay", "ui", "transient"});

  const Stage stage{R"(
label("start", function()
  show("eileen", { layer = "3d" })
  show("lucy", { layer = "ui" })
  show("rain", { layer = "transient" })
  narrate("shown")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  CHECK(stage.scene.layer_of("eileen") == Layer::k3d);
  CHECK(stage.scene.layer_of("lucy") == Layer::kUi);
  CHECK(stage.scene.layer_of("rain") == Layer::kTransient);
  CHECK(stage.scene.layer_of("nobody") == std::nullopt);
}

TEST_CASE("scene: a missing image fails the story at the script line that shows it") {
  const Stage shown{R"(
label("start", function()
  show("eileen")
  show("ghost")
end)
)"};
  CHECK(shown.runtime.state() == RunState::kFailed);
  CHECK(shown.runtime.error() ==
        "scripts/main.lua:4: image 'ghost' not found (no images/ghost.png or images/ghost.jpg)");
  // The failed call changed nothing.
  CHECK(shown.scene.tags(Layer::kCharacters) == Lines{"eileen"});

  const Stage background{R"(
label("start", function()
  scene("bg/classroom")
  show("eileen")

  scene("bg/moon")
end)
)"};
  CHECK(background.runtime.error() ==
        "scripts/main.lua:6: image 'bg/moon' not found (no images/bg/moon.png or "
        "images/bg/moon.jpg)");
  CHECK(path_of(background.scene, "background") == "images/bg/classroom.png");
  CHECK(background.scene.tags(Layer::kCharacters) == Lines{"eileen"});
}

TEST_CASE("scene: a bad argument or option fails the story at its script line") {
  CHECK(line_error(R"(show("eileen", { at = "middle" }))") ==
        "scripts/main.lua:2: unknown position 'middle' (use left, center or right)");
  CHECK(line_error(R"(show("eileen", { layer = "sky" }))") ==
        "scripts/main.lua:2: unknown layer 'sky' (the layers are background, master, "
        "characters, 3d, overlay, ui and transient)");
  CHECK(line_error(R"(show("eileen", { x = "0.5" }))") ==
        "scripts/main.lua:2: bad option 'x' to 'show' (number expected, got string)");
  CHECK(line_error(R"(show("eileen", { y = 0 / 0 }))") ==
        "scripts/main.lua:2: option 'y' must be a finite number");
  CHECK(line_error("show(42)") ==
        "scripts/main.lua:2: bad argument #1 to 'show' (string expected, got number)");
  CHECK(line_error(R"(show("eileen", "left"))") ==
        "scripts/main.lua:2: bad argument #2 to 'show' (table or nil expected, got string)");
  CHECK(line_error("scene()") ==
        "scripts/main.lua:2: bad argument #1 to 'scene' (string expected, got nil)");
  CHECK(line_error("hide({})") ==
        "scripts/main.lua:2: bad argument #1 to 'hide' (string expected, got table)");
}

TEST_CASE("scene: option keys the engine does not know yet are ignored") {
  const Stage stage{R"(
label("start", function()
  scene("bg/classroom", { with = "dissolve" })
  show("eileen", { outfit = "school", face = "smile", at = "right" })
  narrate("shown")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  CHECK(properties_of(stage.scene, "eileen").position == PointF{.x = 960, .y = 720});
}

TEST_CASE("scene: a captured state restores what was shown, where and with which properties") {
  const vnmaki::Vfs vfs = game_files("");
  Scene scene{vfs, kVirtualSize};
  REQUIRE(scene.scene("bg/classroom").empty());
  REQUIRE(scene.show({.tag = "eileen", .image = "eileen_happy", .at = "left"}).empty());
  REQUIRE(scene.show({.tag = "rain", .image = "rain", .layer = "overlay"}).empty());
  // Every property, changed the way a tween will change them.
  constexpr DisplayProperties kChanged{
      .position = {.x = 320, .y = 700},
      .anchor = {.x = 0.25F, .y = 0.75F},
      .scale = {.x = 2, .y = 0.5F},
      .rotation = 30,
      .alpha = 0.5F,
      .tint = vnmaki::rgba8(255, 128, 0, 64),
      .crop = {.x = 0.25F, .y = 0, .width = 0.5F, .height = 1},
  };
  const DisplayProperties changed = kChanged;
  REQUIRE(scene.find("eileen") != nullptr);
  scene.find("eileen")->properties() = changed;

  const Bytes captured = scene.capture();
  scene.hide("eileen");
  REQUIRE(scene.show({.tag = "lucy", .image = "lucy"}).empty());
  REQUIRE(scene.capture() != captured);
  scene.restore(captured);

  CHECK(scene.tags(Layer::kBackground) == Lines{"background"});
  CHECK(path_of(scene, "background") == "images/bg/classroom.png");
  CHECK(scene.tags(Layer::kCharacters) == Lines{"eileen"});
  CHECK(path_of(scene, "eileen") == "images/eileen_happy.png");
  CHECK(properties_of(scene, "eileen") == changed);
  CHECK(scene.tags(Layer::kOverlay) == Lines{"rain"});
  CHECK(scene.capture() == captured);

  SUBCASE("in another scene too") {
    Scene other{vfs, kVirtualSize};
    other.restore(captured);
    CHECK(other.capture() == captured);
    CHECK(properties_of(other, "eileen") == changed);
  }
  SUBCASE("no bytes is the empty scene") {
    scene.restore({});
    for (const Layer layer : vnmaki::kLayerOrder) {
      CHECK(scene.tags(layer).empty());
    }
  }
  SUBCASE("a state cut short is refused and changes nothing") {
    for (std::size_t length = 1; length < captured.size(); ++length) {
      CHECK_THROWS_AS(scene.restore(std::span{captured}.first(length)), std::invalid_argument);
    }
    Bytes longer = captured;
    longer.push_back(std::byte{0});
    CHECK_THROWS_AS(scene.restore(longer), std::invalid_argument);
    CHECK(scene.capture() == captured);
  }
}

TEST_CASE("scene: a replay lands in the scene state live play had") {
  Stage live{std::string{kStory}};
  live.runtime.advance();  // step 2: the menu
  const Bytes scene_at_2 = live.scene.capture();
  live.runtime.choose(1);  // step 3
  REQUIRE_MESSAGE(live.runtime.state() == RunState::kWaiting, live.runtime.error());
  const Bytes scene_at_3 = live.scene.capture();
  REQUIRE(scene_at_2 != scene_at_3);
  CHECK(live.scene.find("eileen") == nullptr);
  CHECK(path_of(live.scene, "lucy") == "images/eileen_happy.png");
  CHECK(properties_of(live.scene, "lucy").position == PointF{.x = 640, .y = 720});
  REQUIRE(live.runtime.label_snapshot() != nullptr);
  const vnmaki::LabelSnapshot snapshot = *live.runtime.label_snapshot();
  const std::vector<vnmaki::InputResult> inputs{live.runtime.input_log().begin(),
                                                live.runtime.input_log().end()};
  const Lines heard_live = live.recorder.commands;
  CHECK(heard_live == Lines{"scene bg/classroom", "show eileen at left", "show lucy at right",
                            "hide eileen", "show lucy image eileen_happy x 0.5"});

  SUBCASE("in the same runtime, silently") {
    live.runtime.replay(snapshot, inputs, 2);
    REQUIRE_MESSAGE(live.runtime.state() == RunState::kWaiting, live.runtime.error());
    CHECK(live.scene.capture() == scene_at_2);
    CHECK(live.scene.tags(Layer::kCharacters) == Lines{"eileen", "lucy"});
    // The scene changed during the replay, but nothing was reported.
    CHECK(live.recorder.commands == heard_live);

    live.runtime.choose(1);
    CHECK(live.scene.capture() == scene_at_3);
  }
  SUBCASE("in a fresh runtime with a fresh scene") {
    Stage fresh{std::string{kStory}};
    fresh.runtime.replay(snapshot, inputs, 3);
    REQUIRE_MESSAGE(fresh.runtime.state() == RunState::kWaiting, fresh.runtime.error());
    CHECK(fresh.scene.capture() == scene_at_3);
  }
  SUBCASE("a malformed scene snapshot fails the replay") {
    vnmaki::LabelSnapshot broken = snapshot;
    broken.scene.pop_back();
    live.runtime.replay(broken, inputs, 2);
    CHECK(live.runtime.state() == RunState::kFailed);
    CHECK(live.runtime.error() ==
          "replay: the scene snapshot of label 'talk' is malformed (scene state: the data ends "
          "early)");
  }
}

TEST_CASE("displayable: the properties place, scale, crop and rotate its bounds") {
  constexpr vnmaki::Size kImageSize{.width = 8, .height = 16};
  constexpr vnmaki::TextureId kSomeTexture{.value = 1};
  constexpr PointF kPlace{.x = 100, .y = 200};
  constexpr PointF kBottomCenter{.x = 0.5F, .y = 1};
  constexpr PointF kDouble{.x = 2, .y = 2};
  constexpr RectF kRightHalf{.x = 0.5F, .y = 0, .width = 0.5F, .height = 1};
  constexpr float kQuarterTurn = 90;
  vnmaki::ImageDisplayable image{"images/eileen.png"};
  vnmaki::Displayable& displayable = image;

  // Before the image is loaded it has no size.
  displayable.properties().position = kPlace;
  CHECK(displayable.bounds() == RectF{.x = 100, .y = 200, .width = 0, .height = 0});

  image.set_texture(kSomeTexture, kImageSize);
  CHECK(displayable.bounds() == RectF{.x = 100, .y = 200, .width = 8, .height = 16});

  displayable.properties().anchor = kBottomCenter;
  CHECK(displayable.bounds() == RectF{.x = 96, .y = 184, .width = 8, .height = 16});

  displayable.properties().scale = kDouble;
  CHECK(displayable.bounds() == RectF{.x = 92, .y = 168, .width = 16, .height = 32});

  displayable.properties().crop = kRightHalf;
  CHECK(displayable.bounds() == RectF{.x = 96, .y = 168, .width = 8, .height = 32});

  // A quarter turn clockwise about the anchor: the box that stood on the anchor now lies to
  // its right.
  displayable.properties().rotation = kQuarterTurn;
  const RectF turned = displayable.bounds();
  CHECK(turned.x == doctest::Approx(100));
  CHECK(turned.y == doctest::Approx(196));
  CHECK(turned.width == doctest::Approx(32));
  CHECK(turned.height == doctest::Approx(8));
}

TEST_CASE("headless: the transcript shows scene, show and hide") {
  const vnmaki::Vfs vfs = game_files(R"(
label("start", function()
  scene("bg/classroom")
  show("eileen", { at = "left" })
  show("lucy", { x = 0.5, y = 0.25 })
  say("Eileen", "hi")
  hide("eileen")
  show("lucy", { image = "eileen_happy", layer = "overlay" })
end)
)");
  Scene scene{vfs, kVirtualSize};
  vnmaki::HeadlessOptions options;
  options.runtime.scene = &scene;
  options.runtime.scene_state = &scene;
  const std::vector<vnmaki::ScriptInput> inputs{vnmaki::AdvanceInput{}};

  const vnmaki::HeadlessResult result = vnmaki::run_headless(vfs, inputs, options);

  CHECK(result.outcome == vnmaki::HeadlessOutcome::kFinished);
  CHECK(result.transcript == Lines{"label start", "scene bg/classroom", "show eileen at left",
                                   "show lucy x 0.5 y 0.25", "say Eileen: hi", "hide eileen",
                                   "show lucy image eileen_happy layer overlay"});
  CHECK(scene.tags(Layer::kOverlay) == Lines{"lucy"});
}

TEST_CASE("headless: a missing image ends the run with the script line") {
  const vnmaki::Vfs vfs = game_files(R"(
label("start", function()
  narrate("before")
  show("ghost")
end)
)");
  Scene scene{vfs, kVirtualSize};
  vnmaki::HeadlessOptions options;
  options.runtime.scene = &scene;
  const std::vector<vnmaki::ScriptInput> inputs{vnmaki::AdvanceInput{}};

  const vnmaki::HeadlessResult result = vnmaki::run_headless(vfs, inputs, options);

  CHECK(result.outcome == vnmaki::HeadlessOutcome::kFailed);
  CHECK(result.transcript == Lines{"label start", "narrate before"});
  CHECK(result.error ==
        "scripts/main.lua:4: image 'ghost' not found (no images/ghost.png or images/ghost.jpg)");
}
