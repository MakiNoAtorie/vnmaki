// Layered sprites (docs/SPEC.md sections 7 and 10), without a GPU: scripts run through
// ScriptRuntime, and a recording renderer shows what the sprite draws. The GPU goldens are in
// tests/render/layered_render_test.cpp.

#include <algorithm>
#include <cstddef>
#include <memory>
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
#include "engine/scene/layered_sprite.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"
#include "tests/support/recording_renderer.hpp"

namespace {

using vnmaki::Layer;
using vnmaki::LayerAttributes;
using vnmaki::LayeredSprite;
using vnmaki::PointF;
using vnmaki::RectF;
using vnmaki::RunState;
using vnmaki::Scene;
using vnmaki::test::RecordingRenderer;
using Lines = std::vector<std::string>;
using Bytes = std::vector<std::byte>;

constexpr vnmaki::Size kVirtualSize{.width = 32, .height = 32};

// Image sizes: the base and the outfits fill the sprite, a face is a strip across its top.
constexpr vnmaki::Size kBodySize{.width = 8, .height = 16};
constexpr vnmaki::Size kFaceSize{.width = 8, .height = 4};
constexpr vnmaki::TextureId kBaseTexture{.value = 1};
constexpr vnmaki::TextureId kSchoolTexture{.value = 2};
constexpr vnmaki::TextureId kSmileTexture{.value = 3};
constexpr vnmaki::TextureId kSadTexture{.value = 4};

constexpr float kHalf = 0.5F;
constexpr float kQuarter = 0.25F;
constexpr float kFade = 0.4F;        // 0.1 s into a 0.25 s crossfade.
constexpr float kFadeHalved = 0.2F;  // The same fade at half alpha.
constexpr double kTick = 0.1;
constexpr double kRest = 0.2;  // The rest of the crossfade.

// The definition of "eileen", and the line the script goes on at.
constexpr std::string_view kDefinition = R"(
layered("eileen", {
  order = { "base", "outfit", "face" },
  base = "eileen/base",
  outfit = { school = "eileen/outfit_school", casual = "eileen/outfit_casual" },
  face = { smile = "eileen/face_smile", sad = "eileen/face_sad" },
  default = { outfit = "school", face = "smile" },
  crossfade = 0.25,
})
)";

vnmaki::Vfs game_files(const std::string& script) {
  return vnmaki::test::memory_vfs({
      {"scripts/main.lua", std::string{kDefinition} + script},
      {"images/eileen/base.png", ""},
      {"images/eileen/outfit_school.png", ""},
      {"images/eileen/outfit_casual.png", ""},
      {"images/eileen/face_smile.png", ""},
      {"images/eileen/face_sad.png", ""},
      {"images/rain.png", ""},
      {"images/lucy.png", ""},
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

  [[nodiscard]] LayeredSprite& sprite(std::string_view tag) {
    auto* found = dynamic_cast<LayeredSprite*>(scene.find(tag));
    REQUIRE(found != nullptr);
    return *found;
  }

  vnmaki::Vfs vfs;
  CommandRecorder recorder;
  Scene scene;
  vnmaki::ScriptRuntime runtime;
};

// The error of a story whose `start` label is the one line `line`.
std::string line_error(const std::string& line) {
  const Stage stage{"label(\"start\", function()\n" + line + "\nend)\n"};
  REQUIRE(stage.runtime.state() == RunState::kFailed);
  return stage.runtime.error();
}

// "scripts/main.lua:<line>: " for the line of the one-line label made by line_error.
std::string at_line() {
  const auto lines = std::ranges::count(kDefinition, '\n');
  return "scripts/main.lua:" + std::to_string(lines + 2) + ": ";
}

// The textures of the images that the tests give a sprite.
void give_textures(LayeredSprite& sprite) {
  sprite.set_texture("images/eileen/base.png", kBaseTexture, kBodySize);
  sprite.set_texture("images/eileen/outfit_school.png", kSchoolTexture, kBodySize);
  sprite.set_texture("images/eileen/face_smile.png", kSmileTexture, kFaceSize);
  sprite.set_texture("images/eileen/face_sad.png", kSadTexture, kFaceSize);
}

LayerAttributes school_smile() { return {{"base", ""}, {"outfit", "school"}, {"face", "smile"}}; }
LayerAttributes school_sad() { return {{"base", ""}, {"outfit", "school"}, {"face", "sad"}}; }

// The progress of a fade, or -1 when the group does not fade.
float progress(const LayeredSprite& sprite, std::string_view group) {
  return sprite.fade_progress(group).value_or(-1);
}

// A story that only has a `start` label.
constexpr std::string_view kIdle = "label(\"start\", function() narrate(\"idle\") end)";

}  // namespace

TEST_CASE("layered: show chooses the defaults, then changes only the groups it names") {
  Stage stage{R"(
label("start", function()
  show("eileen", { at = "left" })
  narrate("defaults")
  show("eileen", { face = "sad" })
  narrate("face")
  show("eileen", { outfit = "casual", x = 0.5 })
  narrate("outfit")
  show("twin", { image = "eileen", outfit = "casual" })
  narrate("twin")
  hide("eileen")
  show("eileen")
  narrate("again")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  CHECK(stage.sprite("eileen").attributes() == school_smile());
  CHECK(stage.scene.find("eileen")->properties().position == PointF{.x = 8, .y = 32});

  stage.runtime.advance();
  CHECK(stage.sprite("eileen").attributes() == school_sad());

  stage.runtime.advance();
  const LayerAttributes casual_sad{{"base", ""}, {"outfit", "casual"}, {"face", "sad"}};
  CHECK(stage.sprite("eileen").attributes() == casual_sad);
  // The call kept the place the sprite had and moved it where it said.
  CHECK(stage.scene.find("eileen")->properties().position == PointF{.x = 16, .y = 32});

  stage.runtime.advance();
  const LayerAttributes casual_smile{{"base", ""}, {"outfit", "casual"}, {"face", "smile"}};
  CHECK(stage.sprite("twin").attributes() == casual_smile);
  CHECK(stage.sprite("twin").name() == "eileen");
  CHECK(stage.scene.tags(Layer::kCharacters) == Lines{"eileen", "twin"});

  // A sprite that was hidden starts again from the defaults.
  stage.runtime.advance();
  CHECK(stage.sprite("eileen").attributes() == school_smile());
}

TEST_CASE("layered: an unknown group or value, or a bad definition, fails at the script line") {
  CHECK(line_error(R"(show("eileen", { hair = "red" }))") ==
        at_line() + "unknown group 'hair' for 'eileen' (the groups are base, outfit, face)");
  CHECK(line_error(R"(show("eileen", { face = "angry" }))") ==
        at_line() +
            "unknown value 'angry' for group 'face' of 'eileen' (the values are sad, smile)");
  CHECK(line_error(R"(show("eileen", { base = "x" }))") ==
        at_line() + "unknown value 'x' for group 'base' of 'eileen' (it has one fixed image)");
  CHECK(line_error(R"(show("eileen", { face = 1 }))") ==
        at_line() + "bad option 'face' to 'show' (string expected, got number)");
  CHECK(line_error(R"(layered("ghost", { order = { "base" }, base = "nope" }))") ==
        at_line() + "image 'nope' not found (no images/nope.png or images/nope.jpg)");
  CHECK(line_error(
            R"(layered("x", { order = { "base" }, base = "rain", default = { base = "y" } }))") ==
        at_line() + "unknown value 'y' for group 'base' of 'x' (it has one fixed image)");
  CHECK(line_error(
            R"(layered("x", { order = { "base" }, base = "rain", default = { hair = "y" } }))") ==
        at_line() + "bad option 'default' to 'layered' ('hair' is not a group in 'order')");
  CHECK(line_error(R"(layered("x", { order = {} }))") ==
        at_line() + "bad option 'order' to 'layered' (a list of group names expected)");
  CHECK(line_error(R"(layered("x", { order = { "a" }, a = 5 }))") ==
        at_line() + "bad option 'a' to 'layered' (string or table expected, got number)");
  CHECK(line_error(R"(layered("x", { order = { "a" }, a = "rain", b = "rain" }))") ==
        at_line() + "bad option 'b' to 'layered' (not a group in 'order')");
  CHECK(line_error(R"(layered("x", { order = { "a", "a" }, a = "rain" }))") ==
        at_line() + "bad option 'order' to 'layered' (item #2 is not a new group name)");
  CHECK(line_error(R"(layered("x", { order = { "a" }, a = {} }))") ==
        at_line() + "bad option 'a' to 'layered' (it needs at least one value)");
  CHECK(line_error(R"(layered("x", { order = { "a" }, a = "rain", crossfade = "slow" }))") ==
        at_line() + "bad option 'crossfade' to 'layered' (number expected, got string)");
  CHECK(line_error(R"(layered("x", { order = { "a" }, a = "rain", crossfade = -1 }))") ==
        at_line() + "option 'crossfade' must be a number from 0 to 86400");

  SUBCASE("a refused show changes nothing") {
    const Stage stage{R"(
label("start", function()
  show("eileen", { face = "sad" })
  show("eileen", { outfit = "casual", face = "angry" })
end)
)"};
    REQUIRE(stage.runtime.state() == RunState::kFailed);
    const auto* sprite = dynamic_cast<const LayeredSprite*>(stage.scene.find("eileen"));
    REQUIRE(sprite != nullptr);
    CHECK(sprite->attributes() == school_sad());
  }
}

TEST_CASE("layered: a changed group fades in on the scene clock, and a replay does not fade") {
  Stage stage{R"(
label("start", function()
  show("eileen")
  say("Eileen", "one")
  show("eileen", { face = "sad" })
  say("Eileen", "two")
  show("eileen", { x = 0.5 })
  say("Eileen", "three")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  CHECK(progress(stage.sprite("eileen"), "face") == -1);

  stage.runtime.advance();
  const Bytes at_two = stage.scene.capture();
  REQUIRE(stage.runtime.label_snapshot() != nullptr);
  const vnmaki::LabelSnapshot snapshot = *stage.runtime.label_snapshot();
  CHECK(progress(stage.sprite("eileen"), "face") == 0);
  CHECK(progress(stage.sprite("eileen"), "outfit") == -1);
  stage.scene.update(kTick);
  CHECK(progress(stage.sprite("eileen"), "face") == doctest::Approx(kFade));

  // A call that changes no group does not restart the fade.
  stage.runtime.advance();
  CHECK(progress(stage.sprite("eileen"), "face") == doctest::Approx(kFade));
  stage.scene.update(kRest);
  CHECK(progress(stage.sprite("eileen"), "face") == -1);

  SUBCASE("a replay of the change is a cut") {
    stage.runtime.replay(snapshot, {}, 2);
    REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
    CHECK(stage.sprite("eileen").attributes() == school_sad());
    CHECK(progress(stage.sprite("eileen"), "face") == -1);
    CHECK(stage.scene.capture() == at_two);
  }
}

TEST_CASE("layered: the snapshot keeps the attributes, the properties and the drawing order") {
  Stage stage{R"(
label("start", function()
  show("lucy")
  show("eileen", { face = "sad", outfit = "casual", at = "right" })
  show("rain")
  show("twin", { image = "eileen", layer = "overlay" })
  narrate("shown")
  show("eileen", { face = "smile" })
  narrate("fading")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  constexpr vnmaki::Color kTinted = vnmaki::rgba8(255, 128, 0, 64);
  stage.scene.find("eileen")->properties().alpha = kHalf;
  stage.scene.find("eileen")->properties().tint = kTinted;
  const Bytes captured = stage.scene.capture();

  const auto check_restored = [&captured](Scene& scene) {
    CHECK(scene.tags(Layer::kCharacters) == Lines{"lucy", "eileen", "rain"});
    CHECK(scene.tags(Layer::kOverlay) == Lines{"twin"});
    const auto* sprite = dynamic_cast<const LayeredSprite*>(scene.find("eileen"));
    REQUIRE(sprite != nullptr);
    const LayerAttributes casual_sad{{"base", ""}, {"outfit", "casual"}, {"face", "sad"}};
    CHECK(sprite->attributes() == casual_sad);
    CHECK(sprite->properties().alpha == kHalf);
    CHECK(sprite->properties().tint == kTinted);
    CHECK(scene.capture() == captured);
  };

  SUBCASE("in the same scene") {
    stage.scene.hide("eileen");
    stage.scene.restore(captured);
    check_restored(stage.scene);
  }
  SUBCASE("in another scene that has the definition") {
    Stage other{std::string{kIdle}};
    other.scene.restore(captured);
    check_restored(other.scene);
  }
  SUBCASE("a fade in progress is saved as its end") {
    stage.runtime.advance();
    stage.scene.update(kTick);
    REQUIRE(progress(stage.sprite("eileen"), "face") >= 0);
    const Bytes later = stage.scene.capture();
    Stage other{std::string{kIdle}};
    other.scene.restore(later);
    const LayerAttributes casual_smile{{"base", ""}, {"outfit", "casual"}, {"face", "smile"}};
    CHECK(other.sprite("eileen").attributes() == casual_smile);
    CHECK(progress(other.sprite("eileen"), "face") == -1);
  }
  SUBCASE("a scene without the definition refuses it") {
    const vnmaki::Vfs vfs = game_files("");
    Scene bare{vfs, kVirtualSize};
    CHECK_THROWS_AS(bare.restore(captured), std::invalid_argument);
  }
  SUBCASE("a state cut short is refused") {
    for (std::size_t length = 1; length < captured.size(); ++length) {
      CHECK_THROWS_AS(stage.scene.restore(std::span{captured}.first(length)),
                      std::invalid_argument);
    }
    CHECK(stage.scene.capture() == captured);
  }
}

TEST_CASE("layered: the layers draw in order at one origin, and the base is the bounds") {
  RecordingRenderer renderer;
  Stage stage{R"(
label("start", function()
  show("eileen")
  narrate("shown")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  LayeredSprite& sprite = stage.sprite("eileen");

  // Without textures it draws nothing and has no size.
  sprite.draw(renderer);
  CHECK(renderer.draws.empty());
  CHECK(sprite.bounds() == RectF{.x = 16, .y = 32, .width = 0, .height = 0});
  CHECK(sprite.missing_paths() == Lines{"images/eileen/base.png", "images/eileen/outfit_school.png",
                                        "images/eileen/face_smile.png"});

  give_textures(sprite);
  CHECK(sprite.missing_paths().empty());
  // The sprite stands on the bottom edge: the base is 8 by 16, centred on x = 16.
  CHECK(sprite.bounds() == RectF{.x = 12, .y = 16, .width = 8, .height = 16});
  sprite.draw(renderer);
  REQUIRE(renderer.draws.size() == 3);
  CHECK(renderer.draws[0].texture == kBaseTexture);
  CHECK(renderer.draws[1].texture == kSchoolTexture);
  CHECK(renderer.draws[2].texture == kSmileTexture);
  // The base and the outfit cover the sprite; the face is the strip at its top.
  CHECK(renderer.draws[0].quad.corners[0] == PointF{.x = 12, .y = 16});
  CHECK(renderer.draws[0].quad.corners[2] == PointF{.x = 20, .y = 32});
  CHECK(renderer.draws[1].quad.corners == renderer.draws[0].quad.corners);
  CHECK(renderer.draws[2].quad.corners[0] == PointF{.x = 12, .y = 16});
  CHECK(renderer.draws[2].quad.corners[2] == PointF{.x = 20, .y = 20});
  CHECK(renderer.draws[2].quad.uv == vnmaki::kWholeTexture);

  SUBCASE("a crop shows the part of each layer it covers") {
    renderer.draws.clear();
    sprite.properties().crop = RectF{.x = 0, .y = kHalf, .width = 1, .height = kHalf};
    sprite.draw(renderer);
    // The strip at the top is outside the lower half.
    REQUIRE(renderer.draws.size() == 2);
    CHECK(renderer.draws[0].quad.uv == RectF{.x = 0, .y = kHalf, .width = 1, .height = kHalf});
    CHECK(renderer.draws[0].quad.corners[0] == PointF{.x = 12, .y = 24});
    CHECK(renderer.draws[0].quad.corners[2] == PointF{.x = 20, .y = 32});
  }
  SUBCASE("a crop through the face shows part of it") {
    renderer.draws.clear();
    sprite.properties().crop = RectF{.x = 0, .y = kQuarter, .width = 1, .height = kQuarter};
    sprite.draw(renderer);
    // Rows 4 to 8 of the sprite: the face (4 rows high) is outside, the body is inside.
    REQUIRE(renderer.draws.size() == 2);
    CHECK(renderer.draws[0].quad.uv ==
          RectF{.x = 0, .y = kQuarter, .width = 1, .height = kQuarter});
  }
}

TEST_CASE("layered: a fading group draws the old image whole and the new one at the fade") {
  RecordingRenderer renderer;
  Stage stage{R"(
label("start", function()
  show("eileen")
  narrate("shown")
  show("eileen", { face = "sad" })
  narrate("changed")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  stage.runtime.advance();
  LayeredSprite& sprite = stage.sprite("eileen");
  give_textures(sprite);
  stage.scene.update(kTick);
  sprite.properties().alpha = kHalf;

  sprite.draw(renderer);
  REQUIRE(renderer.draws.size() == 4);
  CHECK(renderer.draws[2].texture == kSmileTexture);
  CHECK(renderer.draws[2].quad.tint.a == kHalf);
  CHECK(renderer.draws[3].texture == kSadTexture);
  CHECK(renderer.draws[3].quad.tint.a == doctest::Approx(kFadeHalved));

  // Once the fade ends only the new image draws.
  stage.scene.update(kRest);
  renderer.draws.clear();
  sprite.draw(renderer);
  REQUIRE(renderer.draws.size() == 3);
  CHECK(renderer.draws[2].texture == kSadTexture);
}

TEST_CASE("headless: the transcript shows the groups of a layered show") {
  const vnmaki::Vfs vfs = game_files(R"(
label("start", function()
  show("eileen", { face = "sad", outfit = "casual", at = "left" })
  narrate("shown")
end)
)");
  Scene scene{vfs, kVirtualSize};
  vnmaki::HeadlessOptions options;
  options.runtime.scene = &scene;
  options.runtime.scene_state = &scene;
  const std::vector<vnmaki::ScriptInput> inputs{vnmaki::AdvanceInput{}};

  const vnmaki::HeadlessResult result = vnmaki::run_headless(vfs, inputs, options);

  CHECK(result.outcome == vnmaki::HeadlessOutcome::kFinished);
  CHECK(result.transcript ==
        Lines{"label start", "show eileen at left outfit casual face sad", "narrate shown"});
}

TEST_CASE("layered: tweens move a layered sprite like an image") {
  constexpr double kSecond = 1;
  constexpr float kTarget = 0.25F;
  Stage stage{R"(
label("start", function()
  show("eileen")
  tween("eileen", { alpha = 0.25, x = 0.75, duration = 1 })
  wait("eileen")
  narrate("done")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  CHECK(stage.scene.tweening("eileen"));
  stage.scene.update(kSecond);
  CHECK(stage.scene.find("eileen")->properties().alpha == kTarget);
  CHECK(stage.scene.find("eileen")->properties().position.x == 24);
  CHECK_FALSE(stage.scene.tweening("eileen"));
}
