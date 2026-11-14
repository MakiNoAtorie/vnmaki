// Frame animation (docs/SPEC.md section 10), without a GPU: scripts run through ScriptRuntime and
// a recording renderer shows what the animations draw. The GPU golden is in
// tests/render/frame_anim_render_test.cpp.

#include "engine/scene/frame_animation.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/anim/wait_presenter.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/displayable.hpp"
#include "engine/scene/layered_sprite.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"
#include "tests/support/recording_renderer.hpp"

namespace {

using vnmaki::FrameAnimation;
using vnmaki::Layer;
using vnmaki::LayeredSprite;
using vnmaki::PointF;
using vnmaki::RectF;
using vnmaki::RunState;
using vnmaki::Scene;
using vnmaki::WaitPresenter;
using vnmaki::test::RecordingRenderer;
using Lines = std::vector<std::string>;
using Bytes = std::vector<std::byte>;

constexpr vnmaki::Size kVirtualSize{.width = 32, .height = 32};
constexpr vnmaki::Size kSheetSize{.width = 16, .height = 16};  // 2 x 2 cells of 8 x 8.
constexpr vnmaki::Size kFrameSize{.width = 8, .height = 8};
constexpr vnmaki::Size kBodySize{.width = 8, .height = 16};
constexpr vnmaki::Size kEyesSize{.width = 8, .height = 4};
constexpr vnmaki::TextureId kSheetTexture{.value = 1};
constexpr vnmaki::TextureId kFirstTexture{.value = 2};
constexpr vnmaki::TextureId kSecondTexture{.value = 3};
constexpr vnmaki::TextureId kBaseTexture{.value = 4};
constexpr vnmaki::TextureId kOpenTexture{.value = 5};

constexpr double kQuarterSecond = 0.25;
constexpr double kHalfSecond = 0.5;
constexpr double kThreeQuarterSeconds = 0.75;
constexpr double kTenth = 0.1;
constexpr double kLongTime = 100;
constexpr float kHalf = 0.5F;
constexpr float kQuarter = 0.25F;
constexpr float kThreeQuarters = 0.75F;
constexpr float kRightX = 24;       // "right" is three quarters across the 32 pixels.
constexpr float kLeftQuarterX = 8;  // Where the tween of the test ends.
constexpr float kBottomY = 32;      // Standing on the bottom edge.

// Animations first, as the scene needs: a layered sprite looks its values up when it is made.
//   flame: 3 images at 4 fps, loops (the clock wraps at 0.75 s)
//   burst: 3 cells of a 2 x 2 sheet at 2 fps, plays once (1.5 s)
//   blink and wink: 2 images at 4 fps; blink loops, wink plays once (0.5 s)
constexpr std::string_view kDefinitions = R"(
animation("flame", { frames = { "f/a", "f/b", "f/c" }, fps = 4 })
animation("burst", { sheet = "sheet", columns = 2, rows = 2, count = 3, fps = 2, loop = false })
animation("blink", { frames = { "eileen/blink1", "eileen/blink2" }, fps = 4 })
animation("wink", { frames = { "eileen/blink1", "eileen/blink2" }, fps = 4, loop = false })
layered("eileen", {
  order = { "base", "eyes" },
  base = "eileen/base",
  eyes = { open = "eileen/eyes_open", blink = "blink", wink = "wink" },
  default = { eyes = "open" },
})
)";

vnmaki::Vfs game_files(const std::string& script) {
  return vnmaki::test::memory_vfs({
      {"scripts/main.lua", std::string{kDefinitions} + script},
      {"images/f/a.png", ""},
      {"images/f/b.png", ""},
      {"images/f/c.png", ""},
      {"images/sheet.png", ""},
      {"images/eileen/base.png", ""},
      {"images/eileen/eyes_open.png", ""},
      {"images/eileen/blink1.png", ""},
      {"images/eileen/blink2.png", ""},
      {"images/rain.png", ""},
      {"images/lucy.png", ""},
  });
}

// A script started at its `start` label, with a scene.
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

  [[nodiscard]] FrameAnimation& animation(std::string_view tag) {
    auto* found = dynamic_cast<FrameAnimation*>(scene.find(tag));
    REQUIRE(found != nullptr);
    return *found;
  }

  [[nodiscard]] LayeredSprite& sprite(std::string_view tag) {
    auto* found = dynamic_cast<LayeredSprite*>(scene.find(tag));
    REQUIRE(found != nullptr);
    return *found;
  }

  vnmaki::Vfs vfs;
  Scene scene;
  vnmaki::ScriptRuntime runtime;
};

// A story that only has a `start` label.
constexpr std::string_view kIdle = "label(\"start\", function() narrate(\"idle\") end)";

// The error of a story whose `start` label is the one line `line`.
std::string line_error(const std::string& line) {
  const Stage stage{"label(\"start\", function()\n" + line + "\nend)\n"};
  REQUIRE(stage.runtime.state() == RunState::kFailed);
  return stage.runtime.error();
}

// "scripts/main.lua:<line>: " for the line of the one-line label made by line_error.
std::string at_line() {
  const auto lines = std::ranges::count(kDefinitions, '\n');
  return "scripts/main.lua:" + std::to_string(lines + 2) + ": ";
}

// The textures of the three images of "flame".
void give_flame_textures(FrameAnimation& animation) {
  animation.set_texture("images/f/a.png", kFirstTexture, kFrameSize);
  animation.set_texture("images/f/b.png", kSecondTexture, kFrameSize);
  animation.set_texture("images/f/c.png", kSheetTexture, kFrameSize);
}

// The textures that the layered sprite "eileen" needs to draw its base and its eyes.
void give_eileen_textures(LayeredSprite& sprite) {
  sprite.set_texture("images/eileen/base.png", kBaseTexture, kBodySize);
  sprite.set_texture("images/eileen/eyes_open.png", kOpenTexture, kEyesSize);
  sprite.set_texture("images/eileen/blink1.png", kFirstTexture, kEyesSize);
  sprite.set_texture("images/eileen/blink2.png", kSecondTexture, kEyesSize);
}

}  // namespace

TEST_CASE("frame animation: the frame follows the clock, wraps or holds the last") {
  Stage stage{R"(
label("start", function()
  show("flame")
  show("burst")
  narrate("shown")
  show("flame")
  narrate("again")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  FrameAnimation& flame = stage.animation("flame");
  FrameAnimation& burst = stage.animation("burst");
  CHECK(flame.frame() == 0);
  CHECK(burst.frame() == 0);

  stage.scene.update(kQuarterSecond);  // flame: 4 fps; burst: 2 fps
  CHECK(flame.frame() == 1);
  CHECK(burst.frame() == 0);
  stage.scene.update(kQuarterSecond);
  CHECK(flame.frame() == 2);
  CHECK(burst.frame() == 1);
  stage.scene.update(kQuarterSecond);
  CHECK(flame.frame() == 0);  // wrapped
  CHECK(burst.frame() == 1);
  stage.scene.update(kQuarterSecond);
  CHECK(flame.frame() == 1);
  CHECK(burst.frame() == 2);
  // A loop goes on, an animation that plays once holds its last frame.
  stage.scene.update(kLongTime);
  CHECK(burst.frame() == 2);
  CHECK(flame.frame() == 2);  // 101 s * 4 fps = 404 frames, and 404 % 3 = 2
}

TEST_CASE("frame animation: the frame does not depend on the step size") {
  Stage stage{R"(
label("start", function()
  show("flame")
  narrate("shown")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  FrameAnimation& flame = stage.animation("flame");
  const auto run = [&](int steps) {
    for (int i = 0; i < steps; ++i) {
      stage.scene.update(kTenth);
    }
  };
  run(3);
  CHECK(flame.frame() == 1);  // 0.3 s * 4 fps = 1.2 frames
  run(5);
  CHECK(flame.frame() == 0);  // 0.8 s: 3.2 frames, and 3 % 3 = 0
  // Ten steps of 0.1 s add up to a hair under 1 s. That is still frame 4 (4 % 3 = 1).
  run(2);
  CHECK(flame.frame() == 1);
}

TEST_CASE("frame animation: a sheet frame draws its cell, an image frame its own file") {
  RecordingRenderer renderer;
  Stage stage{R"(
label("start", function()
  show("burst")
  narrate("shown")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  FrameAnimation& burst = stage.animation("burst");

  // Without textures it draws nothing and has no size.
  burst.draw(renderer);
  CHECK(renderer.draws.empty());
  CHECK(burst.bounds() == RectF{.x = 16, .y = 32, .width = 0, .height = 0});
  CHECK(burst.missing_paths() == Lines{"images/sheet.png"});

  burst.set_texture("images/sheet.png", kSheetTexture, kSheetSize);
  CHECK(burst.missing_paths().empty());
  // The default place is the bottom centre; a cell is 8 by 8 pixels.
  CHECK(burst.bounds() == RectF{.x = 12, .y = 24, .width = 8, .height = 8});

  const auto uv_of_frame = [&](double seconds) {
    stage.scene.update(seconds);
    renderer.draws.clear();
    burst.draw(renderer);
    REQUIRE(renderer.draws.size() == 1);
    CHECK(renderer.draws[0].texture == kSheetTexture);
    CHECK(renderer.draws[0].quad.corners[0] == PointF{.x = 12, .y = 24});
    CHECK(renderer.draws[0].quad.corners[2] == PointF{.x = 20, .y = 32});
    return renderer.draws[0].quad.uv;
  };
  // Cells go row by row: top left, top right, bottom left.
  CHECK(uv_of_frame(0) == RectF{.x = 0, .y = 0, .width = kHalf, .height = kHalf});
  CHECK(uv_of_frame(kHalfSecond) == RectF{.x = kHalf, .y = 0, .width = kHalf, .height = kHalf});
  CHECK(uv_of_frame(kHalfSecond) == RectF{.x = 0, .y = kHalf, .width = kHalf, .height = kHalf});

  SUBCASE("a crop shows the part of the cell") {
    burst.properties().crop = RectF{.x = 0, .y = kHalf, .width = 1, .height = kHalf};
    renderer.draws.clear();
    burst.draw(renderer);
    REQUIRE(renderer.draws.size() == 1);
    CHECK(renderer.draws[0].quad.uv ==
          RectF{.x = 0, .y = kThreeQuarters, .width = kHalf, .height = kQuarter});
    CHECK(renderer.draws[0].quad.corners[0] == PointF{.x = 12, .y = 28});
  }
}

TEST_CASE("frame animation: an image sequence draws the whole file of the frame") {
  RecordingRenderer renderer;
  Stage stage{R"(
label("start", function()
  show("flame")
  narrate("shown")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  FrameAnimation& flame = stage.animation("flame");
  CHECK(flame.missing_paths() == Lines{"images/f/a.png", "images/f/b.png", "images/f/c.png"});
  give_flame_textures(flame);
  CHECK(flame.missing_paths().empty());
  renderer.draws.clear();
  flame.draw(renderer);
  REQUIRE(renderer.draws.size() == 1);
  CHECK(renderer.draws[0].texture == kFirstTexture);
  CHECK(renderer.draws[0].quad.uv == vnmaki::kWholeTexture);
  stage.scene.update(kQuarterSecond);
  renderer.draws.clear();
  flame.draw(renderer);
  CHECK(renderer.draws[0].texture == kSecondTexture);
}

TEST_CASE("frame animation: wait ends when an animation that plays once has played") {
  Stage stage{R"(
label("start", function()
  show("burst")
  show("flame")
  wait("burst")
  narrate("done")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  WaitPresenter waits{stage.runtime, stage.scene};
  REQUIRE(waits.waiting());
  CHECK(stage.scene.tweening("burst"));
  CHECK_FALSE(stage.scene.tweening("flame"));  // a loop never blocks
  CHECK(stage.scene.tweening(""));
  CHECK_FALSE(waits.update());
  stage.scene.update(kThreeQuarterSeconds);
  CHECK_FALSE(waits.update());
  stage.scene.update(kThreeQuarterSeconds);  // 1.5 s: three frames at 2 fps
  CHECK(waits.update());
  CHECK(std::holds_alternative<vnmaki::NarrateRequest>(*stage.runtime.request()));
  CHECK(stage.animation("burst").frame() == 2);
  CHECK_FALSE(stage.scene.tweening(""));

  SUBCASE("a click goes to the last frame") {
    Stage clicked{R"(
label("start", function()
  show("burst")
  wait_all()
  narrate("done")
end)
)"};
    REQUIRE_MESSAGE(clicked.runtime.state() == RunState::kWaiting, clicked.runtime.error());
    CHECK(clicked.animation("burst").frame() == 0);
    WaitPresenter click{clicked.runtime, clicked.scene};
    CHECK(click.skip());
    CHECK(clicked.animation("burst").frame() == 2);
    CHECK_FALSE(clicked.scene.tweening(""));
  }
}

TEST_CASE("frame animation: a value of a layered group can be an animation") {
  RecordingRenderer renderer;
  Stage stage{R"(
label("start", function()
  show("eileen")
  narrate("open")
  show("eileen", { eyes = "blink" })
  narrate("blinking")
  show("eileen", { eyes = "wink" })
  wait("eileen")
  narrate("winked")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  // A show replaces the sprite object, so each use looks it up again.
  const auto sprite = [&stage]() -> LayeredSprite& { return stage.sprite("eileen"); };
  give_eileen_textures(sprite());
  CHECK_FALSE(sprite().animating());

  stage.runtime.advance();
  CHECK(sprite().attributes() == vnmaki::LayerAttributes{{"base", ""}, {"eyes", "blink"}});
  CHECK(sprite().missing_paths().empty());
  const auto eyes_texture = [&] {
    renderer.draws.clear();
    sprite().draw(renderer);
    REQUIRE(renderer.draws.size() == 2);
    CHECK(renderer.draws[0].texture == kBaseTexture);
    // The eyes are a strip across the top of the sprite, as for a still image.
    CHECK(renderer.draws[1].quad.corners[0] == PointF{.x = 12, .y = 16});
    CHECK(renderer.draws[1].quad.corners[2] == PointF{.x = 20, .y = 20});
    return renderer.draws[1].texture;
  };
  CHECK(eyes_texture() == kFirstTexture);
  stage.scene.update(kQuarterSecond);
  CHECK(eyes_texture() == kSecondTexture);
  stage.scene.update(kQuarterSecond);
  CHECK(eyes_texture() == kFirstTexture);  // blink loops
  CHECK_FALSE(sprite().animating());

  // A value that plays once starts at frame 1, and `wait` waits for it.
  stage.runtime.advance();
  REQUIRE(stage.runtime.state() == RunState::kWaiting);
  CHECK(eyes_texture() == kFirstTexture);
  CHECK(sprite().animating());
  WaitPresenter waits{stage.runtime, stage.scene};
  stage.scene.update(kQuarterSecond);
  CHECK(eyes_texture() == kSecondTexture);
  CHECK_FALSE(waits.update());
  stage.scene.update(kQuarterSecond);
  CHECK(waits.update());
  CHECK(eyes_texture() == kSecondTexture);  // held

  SUBCASE("a click goes to the last frame") {
    Stage clicked{R"(
label("start", function()
  show("eileen", { eyes = "wink" })
  wait("eileen")
  narrate("winked")
end)
)"};
    REQUIRE_MESSAGE(clicked.runtime.state() == RunState::kWaiting, clicked.runtime.error());
    give_eileen_textures(clicked.sprite("eileen"));
    WaitPresenter click{clicked.runtime, clicked.scene};
    CHECK(click.skip());
    renderer.draws.clear();
    clicked.sprite("eileen").draw(renderer);
    REQUIRE(renderer.draws.size() == 2);
    CHECK(renderer.draws[1].texture == kSecondTexture);
  }
}

TEST_CASE("frame animation: the snapshot keeps the place, and an animation restarts at frame 1") {
  Stage stage{R"(
label("start", function()
  show("lucy")
  show("flame", { at = "right" })
  show("rain")
  show("eileen", { eyes = "blink" })
  show("spare", { image = "flame", layer = "overlay" })
  narrate("shown")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  stage.scene.update(kQuarterSecond);
  REQUIRE(stage.animation("flame").frame() == 1);
  stage.scene.find("flame")->properties().alpha = kHalf;
  const Bytes captured = stage.scene.capture();

  const auto check_restored = [&captured](Scene& scene) {
    CHECK(scene.tags(Layer::kCharacters) == Lines{"lucy", "flame", "rain", "eileen"});
    CHECK(scene.tags(Layer::kOverlay) == Lines{"spare"});
    const auto* flame = dynamic_cast<const FrameAnimation*>(scene.find("flame"));
    REQUIRE(flame != nullptr);
    CHECK(flame->name() == "flame");
    CHECK(flame->frame() == 0);
    CHECK(flame->properties().alpha == kHalf);
    CHECK(flame->properties().position == PointF{.x = kRightX, .y = kBottomY});
    const auto* sprite = dynamic_cast<const LayeredSprite*>(scene.find("eileen"));
    REQUIRE(sprite != nullptr);
    CHECK(sprite->attributes() == vnmaki::LayerAttributes{{"base", ""}, {"eyes", "blink"}});
    CHECK(scene.capture() == captured);
  };

  SUBCASE("in the same scene") {
    stage.scene.hide("flame");
    stage.scene.restore(captured);
    check_restored(stage.scene);
  }
  SUBCASE("in another scene that has the definitions") {
    Stage other{std::string{kIdle}};
    other.scene.restore(captured);
    check_restored(other.scene);
  }
  SUBCASE("a scene without the definitions refuses it") {
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

TEST_CASE("frame animation: show again restarts the animation and keeps the tween") {
  Stage stage{R"(
label("start", function()
  show("flame")
  tween("flame", { x = 0.25, duration = 1 })
  narrate("one")
  show("flame")
  narrate("two")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  stage.scene.update(kHalfSecond);
  CHECK(stage.animation("flame").frame() == 2);
  CHECK(stage.scene.find("flame")->properties().position.x > kLeftQuarterX);
  // A new show plays the animation from frame 1 again and keeps what the tween moved.
  const float moved = stage.scene.find("flame")->properties().position.x;
  stage.runtime.advance();
  CHECK(stage.animation("flame").frame() == 0);
  CHECK(stage.scene.find("flame")->properties().position.x == moved);
}

TEST_CASE("frame animation: a bad definition fails at the script line") {
  const std::string bad_image = at_line() +
                                "image 'nope' not found (no images/nope.png, "
                                "images/nope.jpg or images/nope.webp)";
  CHECK(line_error(R"(animation("x", { frames = { "rain", "nope" }, fps = 4 }))") == bad_image);
  CHECK(line_error(R"(animation("x", { sheet = "nope", columns = 1, rows = 1, fps = 4 }))") ==
        bad_image);

  const std::string bad_rate = at_line() + "option 'fps' must be a number above 0 and up to 1000";
  CHECK(line_error(R"(animation("x", { frames = { "rain" } }))") == bad_rate);
  CHECK(line_error(R"(animation("x", { frames = { "rain" }, fps = 0 }))") == bad_rate);
  CHECK(line_error(R"(animation("x", { frames = { "rain" }, fps = 1001 }))") == bad_rate);

  const std::string neither = at_line() +
                              "an animation needs the option 'frames' or the option 'sheet', and "
                              "not both";
  CHECK(line_error(R"(animation("x", { fps = 4 }))") == neither);
  CHECK(line_error(R"(animation("x", { frames = {}, fps = 4 }))") == neither);
  CHECK(line_error(R"(animation("x", { frames = { "rain" }, sheet = "sheet", fps = 4 }))") ==
        neither);

  CHECK(line_error(R"(animation("x", { sheet = "sheet", rows = 2, fps = 4 }))") ==
        at_line() + "option 'columns' must be a whole number from 1 to 4096");
  CHECK(line_error(R"(animation("x", { sheet = "sheet", columns = 1.5, rows = 2, fps = 4 }))") ==
        at_line() + "option 'columns' must be a whole number from 1 to 4096");
  CHECK(line_error(R"(animation("x", { sheet = "sheet", columns = 2, rows = 0, fps = 4 }))") ==
        at_line() + "option 'rows' must be a whole number from 1 to 4096");
  CHECK(line_error(
            R"(animation("x", { sheet = "sheet", columns = 2, rows = 2, count = 5, fps = 4 }))") ==
        at_line() + "option 'count' must be a whole number from 1 to 4");

  CHECK(line_error(R"(animation("x", { frames = { "rain" }, fps = "fast" }))") ==
        at_line() + "bad option 'fps' to 'animation' (number expected, got string)");
  CHECK(line_error(R"(animation("x", { frames = { "rain" }, fps = 4, loop = 1 }))") ==
        at_line() + "bad option 'loop' to 'animation' (boolean expected, got number)");
  CHECK(line_error(R"(animation("x", { frames = { "rain", 2 }, fps = 4 }))") ==
        at_line() + "bad option 'frames' to 'animation' (item #2 is not a string)");
  CHECK(line_error(R"(animation(1, {}))") ==
        at_line() + "bad argument #1 to 'animation' (string expected, got number)");
  CHECK(line_error(R"(animation("x"))") ==
        at_line() + "bad argument #2 to 'animation' (table expected, got nil)");
  CHECK(line_error(R"(animation("", { frames = { "rain" }, fps = 4 }))") ==
        at_line() + "an animation needs a name");

  SUBCASE("a sheet that is all cells needs no count") {
    const Stage stage{R"(
animation("all", { sheet = "sheet", columns = 2, rows = 2, fps = 4 })
label("start", function()
  show("all")
  narrate("shown")
end)
)"};
    CHECK(stage.runtime.state() == RunState::kWaiting);
  }
}
