// Lip sync (docs/SPEC.md section 10): the voice level of the audio module, and the mouth of a
// layered sprite that follows it. Audio is stepped by hand, so a fake clock moves both.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "engine/audio/audio.hpp"
#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/layered_sprite.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"
#include "tests/support/recording_renderer.hpp"

namespace {

using vnmaki::Audio;
using vnmaki::AudioOptions;
using vnmaki::AudioOutput;
using vnmaki::LayeredSprite;
using vnmaki::RunState;

constexpr vnmaki::Size kVirtualSize{.width = 32, .height = 32};

// The clip is 0.2 s of silence, 0.4 s of a tone and 0.2 s of silence. One step is 10 ms; the level
// is the RMS of the last 20 ms, so it changes at the end of the window that has the change.
constexpr double kStep = 0.01;
constexpr int kToneStart = 20;  // steps
constexpr int kToneEnd = 60;
constexpr int kAfterClip = 90;

// The amplitude of the tone is 0.5: its RMS level is 0.5 / sqrt(2).
constexpr double kToneLevel = 0.3535;
constexpr double kLevelTolerance = 0.01;
constexpr double kHalfSecond = 0.5;

constexpr std::string_view kCharacter = R"(
local eileen = character { name = "Eileen", voice_tag = "eileen" }
local bob = character { name = "Bob", voice_tag = "bob" }
)";

std::string definition(const std::string& thresholds) {
  return R"(
layered("eileen", {
  order = { "base", "mouth" },
  base = "eileen/base",
  mouth = { closed = "eileen/closed", half = "eileen/half", open = "eileen/open" },
  lip_sync = { group = "mouth", frames = { "closed", "half", "open" },
               thresholds = )" +
         thresholds + R"(, voice_tag = "eileen" },
})
)";
}

std::string clip() {
  std::ifstream file{VNMAKI_FIXTURES_DIR "/lipsync/voice.wav", std::ios::binary};
  if (!file) {
    throw std::runtime_error{"missing the lipsync fixture"};
  }
  return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

vnmaki::Vfs game_files(const std::string& script) {
  return vnmaki::test::memory_vfs({
      {"scripts/main.lua", script},
      {"audio/voice/eileen/001.wav", clip()},
      {"images/eileen/base.png", ""},
      {"images/eileen/closed.png", ""},
      {"images/eileen/half.png", ""},
      {"images/eileen/open.png", ""},
  });
}

// "eileen" shown, a voice, and two lines: the first is Eileen's, the second is Bob's.
std::string story(const std::string& thresholds = "{ 0.05, 0.2 }") {
  return std::string{kCharacter} + definition(thresholds) + R"(
label("start", function()
  show("eileen")
  voice("eileen/001")
  say(eileen, "Hello")
  say(bob, "Hi")
end)
)";
}

struct Stage {
  explicit Stage(const std::string& script)
      : vfs(game_files(script)),
        scene(vfs, kVirtualSize),
        audio(vfs),
        state({&scene, &audio}),
        runtime(vfs, options()) {
    scene.set_voice_level(&audio);
    runtime.load_scripts();
    runtime.start("start");
  }

  [[nodiscard]] vnmaki::ScriptRuntimeOptions options() {
    vnmaki::ScriptRuntimeOptions result;
    result.scene = &scene;
    result.audio = &audio;
    result.scene_state = &state;
    return result;
  }

  [[nodiscard]] LayeredSprite& sprite() {
    auto* found = dynamic_cast<LayeredSprite*>(scene.find("eileen"));
    REQUIRE(found != nullptr);
    return *found;
  }

  // Moves the fake clock of the audio and the scene by `steps` of 10 ms.
  void run(int steps) {
    for (int i = 0; i < steps; ++i) {
      audio.update(kStep);
      scene.update(kStep);
    }
  }

  [[nodiscard]] std::string mouth() { return std::string{sprite().lip_frame()}; }

  vnmaki::Vfs vfs;
  vnmaki::Scene scene;
  Audio audio;
  vnmaki::CompositeSceneState state;
  vnmaki::ScriptRuntime runtime;
};

// The error of a story whose `start` label defines a sprite with this `lip_sync` table.
std::string lip_sync_error(const std::string& lip_sync) {
  const Stage stage{R"(
label("start", function()
  layered("eileen", {
    order = { "base", "mouth" },
    base = "eileen/base",
    mouth = { closed = "eileen/closed", open = "eileen/open" },
    lip_sync = )" + lip_sync + R"(,
  })
end)
)"};
  REQUIRE(stage.runtime.state() == RunState::kFailed);
  return stage.runtime.error();
}

}  // namespace

TEST_CASE("lip sync: the mouth is closed, then open, then closed at the expected times") {
  Stage stage{story()};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  REQUIRE(stage.audio.voice_playing());
  CHECK(stage.mouth() == "closed");

  stage.run(kToneStart);
  CHECK(stage.mouth() == "closed");  // the last 20 ms are still silence
  stage.run(2);
  CHECK(stage.mouth() == "open");
  stage.run(kToneEnd - kToneStart - 2);
  CHECK(stage.mouth() == "open");  // the last 20 ms are the tone
  stage.run(2);
  CHECK(stage.mouth() == "closed");

  stage.run(kAfterClip - kToneEnd - 2);
  CHECK_FALSE(stage.audio.voice_playing());
  CHECK(stage.mouth() == "closed");
}

TEST_CASE("lip sync: the group draws only the frame of the mouth") {
  constexpr vnmaki::TextureId kBase{.value = 1};
  constexpr vnmaki::TextureId kClosed{.value = 2};
  constexpr vnmaki::TextureId kOpen{.value = 3};
  constexpr vnmaki::Size kSize{.width = 8, .height = 8};
  Stage stage{story()};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  LayeredSprite& sprite = stage.sprite();
  CHECK(sprite.missing_paths() ==
        std::vector<std::string>{"images/eileen/base.png", "images/eileen/closed.png"});
  sprite.set_texture("images/eileen/base.png", kBase, kSize);
  sprite.set_texture("images/eileen/closed.png", kClosed, kSize);
  sprite.set_texture("images/eileen/open.png", kOpen, kSize);

  vnmaki::test::RecordingRenderer renderer;
  sprite.draw(renderer);
  REQUIRE(renderer.draws.size() == 2);
  CHECK(renderer.draws[1].texture == kClosed);
  stage.run(kToneStart + 2);
  renderer.draws.clear();
  sprite.draw(renderer);
  REQUIRE(renderer.draws.size() == 2);
  CHECK(renderer.draws[1].texture == kOpen);
}

TEST_CASE("lip sync: a quieter level picks a middle frame") {
  Stage stage{story("{ 0.05, 0.5 }")};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  stage.run(kToneStart + 2);
  CHECK(stage.mouth() == "half");  // 0.35 is above 0.05 and below 0.5
}

TEST_CASE("lip sync: the audio level follows the tag of the voice") {
  Stage stage{story()};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  stage.run(kToneStart + 2);
  const std::optional<double> level = stage.audio.voice_level("eileen");
  REQUIRE(level.has_value());
  CHECK(*level == doctest::Approx(kToneLevel).epsilon(kLevelTolerance));
  // Another tag, and no tag, hear nothing.
  CHECK_FALSE(stage.audio.voice_level("bob").has_value());
  CHECK_FALSE(stage.audio.voice_level("").has_value());

  // After the voice: nothing plays under the tag any more.
  stage.run(kAfterClip);
  CHECK_FALSE(stage.audio.voice_level("eileen").has_value());
}

TEST_CASE("lip sync: a voice without a tagged line has no level") {
  const vnmaki::Vfs vfs = game_files("");
  Audio audio{vfs};
  REQUIRE(audio.voice("eileen/001").empty());
  audio.update(kHalfSecond);
  CHECK(audio.voice_playing());
  CHECK_FALSE(audio.voice_level("eileen").has_value());
  audio.tag_voice("eileen");
  CHECK(audio.voice_level("eileen").has_value());
}

TEST_CASE("lip sync: a replay keeps the mouth closed, and the mouth is not in the snapshot") {
  Stage stage{story()};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  const std::vector<std::byte> closed = stage.scene.capture();
  stage.run(kToneStart + 2);
  REQUIRE(stage.mouth() == "open");
  CHECK(stage.scene.capture() == closed);  // the mouth is not state

  // Rolling back replays the label from its start: show and voice run again, and the voice does
  // not play.
  REQUIRE(stage.runtime.roll_back() == false);  // Eileen's line is the first step
  stage.runtime.advance();
  REQUIRE(stage.runtime.roll_back());
  CHECK_FALSE(stage.audio.voice_playing());
  stage.run(kToneStart + 2);
  CHECK(stage.mouth() == "closed");
  CHECK_FALSE(stage.audio.voice_level("eileen").has_value());
}

TEST_CASE("lip sync: bad definitions fail at the script line") {
  CHECK(lip_sync_error("{ group = \"face\", frames = { \"closed\", \"open\" }, "
                       "thresholds = { 0.1 } }")
            .find("'group' must be a group in 'order'") != std::string::npos);
  CHECK(lip_sync_error("{ group = \"mouth\", frames = { \"closed\", \"wide\" }, "
                       "thresholds = { 0.1 } }")
            .find("unknown value 'wide'") != std::string::npos);
  CHECK(lip_sync_error("{ group = \"mouth\", frames = { \"closed\", \"open\" }, "
                       "thresholds = { 0.1, 0.2 } }")
            .find("one threshold fewer than frames") != std::string::npos);
  CHECK(lip_sync_error("{ group = \"mouth\", frames = { \"closed\", \"open\" }, "
                       "thresholds = { 2 } }")
            .find("thresholds that rise") != std::string::npos);
  CHECK(lip_sync_error("{ group = \"mouth\", frames = { \"closed\", \"open\" }, "
                       "thresholds = { \"high\" } }")
            .find("'thresholds' item #1 is not a number") != std::string::npos);
  CHECK(lip_sync_error("{ group = \"mouth\", frames = { \"closed\", \"open\" }, "
                       "thresholds = { 0.1 }, speed = 2 }")
            .find("unknown key 'speed'") != std::string::npos);
  CHECK(lip_sync_error("5").find("(table expected, got number)") != std::string::npos);
}

TEST_CASE("lip sync: the audio level works with the null device") {
  if (!vnmaki::JobSystem::threads_supported()) {
    return;
  }
  const vnmaki::Vfs vfs = game_files("");
  Audio audio{vfs, AudioOptions{.output = AudioOutput::kNullDevice}};
  REQUIRE(audio.voice("eileen/001").empty());
  audio.tag_voice("eileen");

  constexpr std::chrono::seconds kTimeout{10};
  constexpr std::chrono::milliseconds kPoll{2};
  const auto deadline = std::chrono::steady_clock::now() + kTimeout;
  double loudest = 0;
  while (audio.voice_playing() && std::chrono::steady_clock::now() < deadline) {
    loudest = std::max(loudest, audio.voice_level("eileen").value_or(0));
    std::this_thread::sleep_for(kPoll);
  }
  CHECK_FALSE(audio.voice_playing());
  CHECK(loudest == doctest::Approx(kToneLevel).epsilon(kLevelTolerance));
}
