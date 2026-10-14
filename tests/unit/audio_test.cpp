// Audio (docs/SPEC.md sections 7, 8 and 14) with no device: the mixer is stepped by hand and the
// tests check what each channel plays, never the samples.

#include "engine/audio/audio.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "engine/audio/audio_decoder.hpp"
#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/audio_fixtures.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::Audio;
using vnmaki::AudioChannelState;
using vnmaki::AudioLoopPoints;
using vnmaki::AudioOptions;
using vnmaki::AudioOutput;
using vnmaki::MusicRequest;
using vnmaki::RunState;
using vnmaki::SoundRequest;
using vnmaki::test::audio_fixture;

using Lines = std::vector<std::string>;

// Frames at kAudioSampleRate. The music files are two seconds long, the sound and the voice a
// quarter of a second.
constexpr std::uint64_t kSecondFrames = 48'000;
constexpr std::uint64_t kHalfSecondFrames = 24'000;
constexpr std::uint64_t kQuarterSecondFrames = 12'000;

constexpr double kTenth = 0.1;
constexpr double kFifth = 0.2;
constexpr double kQuarter = 0.25;
constexpr double kHalf = 0.5;
constexpr double kThreeQuarters = 0.75;
constexpr double kOneAndQuarter = 1.25;
constexpr double kTwoAndHalf = 2.5;

constexpr vnmaki::Size kVirtualSize{.width = 1280, .height = 720};

// A game with one script and audio files in every format.
vnmaki::Vfs game_files(const std::string& script) {
  return vnmaki::test::memory_vfs({
      {"scripts/main.lua", script},
      {"audio/bgm/theme.wav", audio_fixture("long.wav")},
      {"audio/bgm/night.wav", audio_fixture("long.wav")},
      {"audio/bgm/tagged.ogg", audio_fixture("loop.ogg")},
      {"audio/sfx/door.wav", audio_fixture("tone.wav")},
      {"audio/sfx/steps.flac", audio_fixture("tone.flac")},
      {"audio/voice/eileen/001.opus", audio_fixture("tone.opus")},
      {"audio/broken.ogg", "This is not an Ogg file."},
      {"images/bg/room.png", ""},
  });
}

MusicRequest music_request(const std::string& name, double fade = 0) {
  MusicRequest request;
  request.name = name;
  request.fade = fade;
  return request;
}

SoundRequest sound_request(const std::string& name) {
  SoundRequest request;
  request.name = name;
  return request;
}

// An Audio stepped by hand over the game's files.
struct Mixer {
  explicit Mixer(AudioOptions options = {}) : vfs(game_files("")), audio(vfs, options) {}

  vnmaki::Vfs vfs;
  Audio audio;
};

class CommandRecorder final : public vnmaki::ScriptObserver {
 public:
  void on_audio_command(std::string_view command) override { commands.emplace_back(command); }

  Lines commands;
};

// A script started at its `start` label, with a scene and audio in its snapshots.
struct Stage {
  explicit Stage(const std::string& script)
      : vfs(game_files(script)),
        scene(vfs, kVirtualSize),
        audio(vfs),
        state({&scene, &audio}),
        runtime(vfs, options()) {
    runtime.load_scripts();
    runtime.start("start");
  }

  [[nodiscard]] vnmaki::ScriptRuntimeOptions options() {
    vnmaki::ScriptRuntimeOptions result;
    result.observer = &recorder;
    result.scene = &scene;
    result.audio = &audio;
    result.scene_state = &state;
    return result;
  }

  vnmaki::Vfs vfs;
  CommandRecorder recorder;
  vnmaki::Scene scene;
  Audio audio;
  vnmaki::CompositeSceneState state;
  vnmaki::ScriptRuntime runtime;
};

// The error of a story whose `start` label is the one line `line` (line 2 of the script).
std::string line_error(const std::string& line) {
  const Stage stage{"label(\"start\", function()\n" + line + "\nend)\n"};
  REQUIRE(stage.runtime.state() == RunState::kFailed);
  return stage.runtime.error();
}

}  // namespace

// --- The mixer ---------------------------------------------------------------------------------

TEST_CASE("audio: the named channels exist, silent and at full volume") {
  const Mixer mixer;

  CHECK(mixer.audio.channels() == Lines{"ambient", "music", "sound", "ui", "voice"});
  for (const std::string& name : mixer.audio.channels()) {
    const AudioChannelState channel = mixer.audio.channel(name);
    CHECK(channel.volume == 1);
    CHECK(channel.duck == 1);
    CHECK(channel.music.empty());
    CHECK(channel.tracks.empty());
  }
}

TEST_CASE("audio: music loops from its end to its start") {
  Mixer mixer;
  REQUIRE(mixer.audio.music(music_request("bgm/theme")).empty());

  AudioChannelState music = mixer.audio.channel("music");
  CHECK(music.music == "bgm/theme");
  REQUIRE(music.tracks.size() == 1);
  CHECK(music.tracks[0].name == "bgm/theme");
  CHECK(music.tracks[0].loop);
  CHECK(music.tracks[0].position == 0);
  CHECK(music.tracks[0].gain == 1);

  mixer.audio.update(kTwoAndHalf);  // Half a second into the second time round.

  music = mixer.audio.channel("music");
  REQUIRE(music.tracks.size() == 1);
  CHECK(music.tracks[0].position == kHalfSecondFrames);
}

TEST_CASE("audio: new music cross-fades with the music before it") {
  Mixer mixer;
  REQUIRE(mixer.audio.music(music_request("bgm/theme")).empty());
  mixer.audio.update(1);

  REQUIRE(mixer.audio.music(music_request("bgm/night", 1)).empty());
  mixer.audio.update(kQuarter);

  AudioChannelState music = mixer.audio.channel("music");
  CHECK(music.music == "bgm/night");
  REQUIRE(music.tracks.size() == 2);
  CHECK(music.tracks[0].name == "bgm/theme");
  CHECK(music.tracks[0].fading_out);
  CHECK(music.tracks[0].gain == doctest::Approx(kThreeQuarters));
  CHECK(music.tracks[1].name == "bgm/night");
  CHECK_FALSE(music.tracks[1].fading_out);
  CHECK(music.tracks[1].gain == doctest::Approx(kQuarter));
  CHECK(music.tracks[1].position == kQuarterSecondFrames);

  mixer.audio.update(1);

  music = mixer.audio.channel("music");
  REQUIRE(music.tracks.size() == 1);
  CHECK(music.tracks[0].name == "bgm/night");
  CHECK(music.tracks[0].gain == 1);
}

TEST_CASE("audio: music that is already playing carries on") {
  Mixer mixer;
  REQUIRE(mixer.audio.music(music_request("bgm/theme")).empty());
  mixer.audio.update(kHalf);

  REQUIRE(mixer.audio.music(music_request("bgm/theme", 1)).empty());

  const AudioChannelState music = mixer.audio.channel("music");
  REQUIRE(music.tracks.size() == 1);
  CHECK(music.tracks[0].position == kHalfSecondFrames);
  CHECK(music.tracks[0].gain == 1);
}

TEST_CASE("audio: music loops between the loop points in the file's tags") {
  Mixer mixer;
  // tagged.ogg has LOOPSTART=2400 and LOOPEND=9600.
  REQUIRE(mixer.audio.music(music_request("bgm/tagged")).empty());
  CHECK(mixer.audio.channel("music").tracks.at(0).loop_points ==
        AudioLoopPoints{.start = 2400, .end = 9600});

  // 12000 frames: 9600 to the loop end, then 2400 on from the loop start.
  mixer.audio.update(kQuarter);

  CHECK(mixer.audio.channel("music").tracks.at(0).position == 4800);
}

TEST_CASE("audio: loop points given in the call override the file") {
  Mixer mixer;
  MusicRequest request = music_request("bgm/theme");
  request.loop_start = kHalf;
  request.loop_end = 1;
  REQUIRE(mixer.audio.music(request).empty());
  CHECK(mixer.audio.channel("music").tracks.at(0).loop_points ==
        AudioLoopPoints{.start = kHalfSecondFrames, .end = kSecondFrames});

  // One second to the loop end, then a quarter second on from the loop start.
  mixer.audio.update(kOneAndQuarter);

  CHECK(mixer.audio.channel("music").tracks.at(0).position ==
        kHalfSecondFrames + kQuarterSecondFrames);

  request.loop_end = kQuarter;
  CHECK(mixer.audio.music(request) == "option 'loop_end' must be after 'loop_start'");
}

TEST_CASE("audio: sounds play once, over each other") {
  Mixer mixer;
  REQUIRE(mixer.audio.sound(sound_request("sfx/door")).empty());
  mixer.audio.update(kTenth);
  REQUIRE(mixer.audio.sound(sound_request("sfx/door")).empty());

  AudioChannelState sound = mixer.audio.channel("sound");
  REQUIRE(sound.tracks.size() == 2);
  CHECK_FALSE(sound.tracks[0].loop);
  CHECK(sound.music.empty());

  mixer.audio.update(kFifth);  // The first one is over.

  sound = mixer.audio.channel("sound");
  REQUIRE(sound.tracks.size() == 1);
  CHECK(sound.tracks[0].position == 9600);

  mixer.audio.update(kTenth);

  CHECK(mixer.audio.channel("sound").tracks.empty());
}

TEST_CASE("audio: a sound can play on a channel the script names") {
  Mixer mixer;
  SoundRequest request = sound_request("sfx/steps");
  request.channel = "footsteps";
  REQUIRE(mixer.audio.sound(request).empty());

  CHECK(mixer.audio.channels() == Lines{"ambient", "footsteps", "music", "sound", "ui", "voice"});
  CHECK(mixer.audio.channel("footsteps").tracks.size() == 1);
  CHECK(mixer.audio.channel("sound").tracks.empty());
}

TEST_CASE("audio: a voice replaces the voice before it, and voice_playing follows it") {
  Mixer mixer;
  CHECK_FALSE(mixer.audio.voice_playing());

  REQUIRE(mixer.audio.voice("eileen/001").empty());
  mixer.audio.update(kTenth);
  // The backlog plays a line's voice again with the same call.
  REQUIRE(mixer.audio.voice("eileen/001").empty());

  const AudioChannelState voice = mixer.audio.channel("voice");
  REQUIRE(voice.tracks.size() == 1);
  CHECK(voice.tracks[0].name == "voice/eileen/001");
  CHECK(voice.tracks[0].position == 0);
  CHECK(mixer.audio.voice_playing());

  mixer.audio.update(kHalf);

  CHECK_FALSE(mixer.audio.voice_playing());
}

TEST_CASE("audio: music ducks while a voice plays and comes back after it") {
  constexpr double kDuckVolume = 0.25;
  constexpr double kHalfDucked = 0.625;
  constexpr double kHalfFade = 0.05;
  Mixer mixer{AudioOptions{.duck_volume = kDuckVolume, .duck_fade = kTenth}};
  REQUIRE(mixer.audio.music(music_request("bgm/theme")).empty());
  REQUIRE(mixer.audio.voice("eileen/001").empty());

  mixer.audio.update(kHalfFade);
  CHECK(mixer.audio.channel("music").duck == doctest::Approx(kHalfDucked));

  mixer.audio.update(kTenth);
  CHECK(mixer.audio.channel("music").duck == doctest::Approx(kDuckVolume));
  CHECK(mixer.audio.channel("voice").duck == 1);
  CHECK(mixer.audio.channel("music").volume == 1);

  mixer.audio.update(kHalf);  // The voice is a quarter of a second long.
  CHECK_FALSE(mixer.audio.voice_playing());
  CHECK(mixer.audio.channel("music").duck == 1);
}

TEST_CASE("audio: stop fades a channel out, or cuts it") {
  Mixer mixer;
  REQUIRE(mixer.audio.music(music_request("bgm/theme")).empty());
  REQUIRE(mixer.audio.stop("music", 1).empty());
  mixer.audio.update(kHalf);

  AudioChannelState music = mixer.audio.channel("music");
  CHECK(music.music.empty());
  REQUIRE(music.tracks.size() == 1);
  CHECK(music.tracks[0].fading_out);
  CHECK(music.tracks[0].gain == doctest::Approx(kHalf));

  mixer.audio.update(kThreeQuarters);
  CHECK(mixer.audio.channel("music").tracks.empty());

  REQUIRE(mixer.audio.music(music_request("bgm/theme")).empty());
  REQUIRE(mixer.audio.sound(sound_request("sfx/door")).empty());
  REQUIRE(mixer.audio.stop("music", 0).empty());
  REQUIRE(mixer.audio.stop("sound", 0).empty());
  CHECK(mixer.audio.channel("music").tracks.empty());
  CHECK(mixer.audio.channel("sound").tracks.empty());
}

TEST_CASE("audio: each channel has its own volume") {
  constexpr double kTooLoud = 1.5;
  Mixer mixer;
  REQUIRE(mixer.audio.volume("music", kHalf).empty());
  REQUIRE(mixer.audio.volume("rain", kQuarter).empty());

  CHECK(mixer.audio.channel("music").volume == kHalf);
  CHECK(mixer.audio.channel("rain").volume == kQuarter);
  CHECK(mixer.audio.channel("voice").volume == 1);

  CHECK(mixer.audio.volume("music", kTooLoud) == "volume must be a number from 0 to 1");
  CHECK(mixer.audio.channel("music").volume == kHalf);
}

TEST_CASE("audio: a file that is missing or not audio is reported, and nothing changes") {
  Mixer mixer;
  REQUIRE(mixer.audio.music(music_request("bgm/theme")).empty());

  CHECK(mixer.audio.music(music_request("bgm/ghost")) ==
        "audio 'bgm/ghost' not found (no audio/bgm/ghost.opus, .ogg, .flac or .wav)");
  CHECK(mixer.audio.music(music_request("broken")) ==
        "audio 'broken': audio/broken.ogg is not an Opus, Vorbis, FLAC or WAV file");
  CHECK(mixer.audio.sound(sound_request("sfx/ghost")) ==
        "audio 'sfx/ghost' not found (no audio/sfx/ghost.opus, .ogg, .flac or .wav)");
  CHECK(mixer.audio.voice("eileen/999") ==
        "audio 'voice/eileen/999' not found (no audio/voice/eileen/999.opus, .ogg, .flac or "
        ".wav)");

  const AudioChannelState music = mixer.audio.channel("music");
  CHECK(music.music == "bgm/theme");
  CHECK(music.tracks.size() == 1);
}

TEST_CASE("audio: the null device mixes on its own thread") {
  if (!vnmaki::JobSystem::threads_supported()) {
    CHECK_FALSE(Audio::device_supported());
    return;
  }
  const vnmaki::Vfs vfs = game_files("");
  Audio audio{vfs, AudioOptions{.output = AudioOutput::kNullDevice}};
  REQUIRE(audio.music(music_request("bgm/theme")).empty());
  REQUIRE(audio.voice("eileen/001").empty());

  constexpr std::chrono::seconds kTimeout{10};
  constexpr std::chrono::milliseconds kPoll{5};
  const auto deadline = std::chrono::steady_clock::now() + kTimeout;
  while (audio.voice_playing() && std::chrono::steady_clock::now() < deadline) {
    audio.update(0);
    std::this_thread::sleep_for(kPoll);
  }

  CHECK_FALSE(audio.voice_playing());
  CHECK(audio.channel("music").tracks.at(0).position > 0);
}

// --- Script commands ---------------------------------------------------------------------------

TEST_CASE("audio: music, sound, voice, stop and volume reach the mixer and the observer") {
  const Stage stage{R"(
label("start", function()
  music("bgm/theme", { fade = 1.5 })
  music("bgm/tagged", { channel = "ambient", loop_start = 0.05, loop_end = 0.2 })
  sound("sfx/door")
  sound("sfx/steps", { channel = "ui" })
  voice("eileen/001")
  volume("ambient", 0.5)
  narrate("Listen.")
  stop("ambient", { fade = 2 })
  stop("sound")
  narrate("Quiet.")
end)
)"};
  REQUIRE(stage.runtime.state() == RunState::kWaiting);

  CHECK(stage.recorder.commands == Lines{
                                       "music bgm/theme fade 1.5",
                                       "music bgm/tagged channel ambient loop_start 0.05 "
                                       "loop_end 0.2",
                                       "sound sfx/door",
                                       "sound sfx/steps channel ui",
                                       "voice eileen/001",
                                       "volume ambient 0.5",
                                   });
  const Audio& audio = stage.audio;
  CHECK(audio.channel("music").music == "bgm/theme");
  CHECK(audio.channel("music").tracks.at(0).gain == 0);  // It fades in.
  CHECK(audio.channel("ambient").music == "bgm/tagged");
  CHECK(audio.channel("ambient").tracks.at(0).loop_points ==
        AudioLoopPoints{.start = 2400, .end = 9600});
  CHECK(audio.channel("ambient").volume == kHalf);
  CHECK(audio.channel("sound").tracks.size() == 1);
  CHECK(audio.channel("ui").tracks.size() == 1);
  CHECK(audio.voice_playing());
}

TEST_CASE("audio: stop from a script ends a channel") {
  Stage stage{R"(
label("start", function()
  music("bgm/theme")
  sound("sfx/door")
  narrate("Listen.")
  stop("music", { fade = 2 })
  stop("sound")
  narrate("Quiet.")
end)
)"};
  stage.runtime.advance();
  REQUIRE(stage.runtime.state() == RunState::kWaiting);

  CHECK(stage.recorder.commands ==
        Lines{"music bgm/theme", "sound sfx/door", "stop music fade 2", "stop sound"});
  CHECK(stage.audio.channel("music").music.empty());
  CHECK(stage.audio.channel("music").tracks.at(0).fading_out);
  CHECK(stage.audio.channel("sound").tracks.empty());
}

TEST_CASE("audio: a missing file fails the story at the script line") {
  CHECK(line_error("music(\"bgm/ghost\")") ==
        "scripts/main.lua:2: audio 'bgm/ghost' not found (no audio/bgm/ghost.opus, .ogg, .flac "
        "or .wav)");
  CHECK(line_error("sound(\"sfx/ghost\")") ==
        "scripts/main.lua:2: audio 'sfx/ghost' not found (no audio/sfx/ghost.opus, .ogg, .flac "
        "or .wav)");
  CHECK(line_error("voice(\"nobody\")") ==
        "scripts/main.lua:2: audio 'voice/nobody' not found (no audio/voice/nobody.opus, .ogg, "
        ".flac or .wav)");
  CHECK(line_error("music(\"broken\")") ==
        "scripts/main.lua:2: audio 'broken': audio/broken.ogg is not an Opus, Vorbis, FLAC or "
        "WAV file");
}

TEST_CASE("audio: bad arguments fail the story at the script line") {
  CHECK(line_error("music(7)") ==
        "scripts/main.lua:2: bad argument #1 to 'music' (string expected, got number)");
  CHECK(line_error("music(\"bgm/theme\", { fade = \"slow\" })") ==
        "scripts/main.lua:2: bad option 'fade' to 'music' (number expected, got string)");
  CHECK(line_error("music(\"bgm/theme\", { fade = -1 })") ==
        "scripts/main.lua:2: option 'fade' must be a number from 0 to 86400");
  CHECK(line_error("music(\"bgm/theme\", { loop_start = 0/0 })") ==
        "scripts/main.lua:2: option 'loop_start' must be a number from 0 to 86400");
  CHECK(line_error("sound(\"sfx/door\", { channel = \"\" })") ==
        "scripts/main.lua:2: the channel name is empty");
  CHECK(line_error("stop()") ==
        "scripts/main.lua:2: bad argument #1 to 'stop' (string expected, got nil)");
  CHECK(line_error("stop(\"music\", { fade = 1/0 })") ==
        "scripts/main.lua:2: option 'fade' must be a number from 0 to 86400");
  CHECK(line_error("volume(\"music\")") ==
        "scripts/main.lua:2: bad argument #2 to 'volume' (number expected, got nil)");
  CHECK(line_error("volume(\"music\", 2)") ==
        "scripts/main.lua:2: volume must be a number from 0 to 1");
}

TEST_CASE("audio: the commands work without an Audio, and only reach the observer") {
  const vnmaki::Vfs vfs = game_files(R"(
label("start", function()
  music("bgm/anything")
  voice("anyone")
  narrate("Listen.")
end)
)");
  CommandRecorder recorder;
  vnmaki::ScriptRuntimeOptions options;
  options.observer = &recorder;
  vnmaki::ScriptRuntime runtime{vfs, options};
  runtime.load_scripts();
  runtime.start("start");

  CHECK(runtime.state() == RunState::kWaiting);
  CHECK(recorder.commands == Lines{"music bgm/anything", "voice anyone"});
}

// --- Snapshots and replay ----------------------------------------------------------------------

TEST_CASE("audio: a snapshot keeps each channel's music and volume") {
  Mixer first;
  MusicRequest ambient = music_request("bgm/night");
  ambient.channel = "ambient";
  ambient.loop_start = kHalf;
  REQUIRE(first.audio.music(music_request("bgm/theme")).empty());
  REQUIRE(first.audio.music(ambient).empty());
  REQUIRE(first.audio.volume("music", kHalf).empty());
  REQUIRE(first.audio.volume("rain", kQuarter).empty());
  REQUIRE(first.audio.sound(sound_request("sfx/door")).empty());
  const std::vector<std::byte> snapshot = first.audio.capture();

  Mixer second;
  second.audio.restore(snapshot);
  second.audio.update(0);

  CHECK(second.audio.capture() == snapshot);
  CHECK(second.audio.channel("music").music == "bgm/theme");
  CHECK(second.audio.channel("music").volume == kHalf);
  CHECK(second.audio.channel("music").tracks.at(0).name == "bgm/theme");
  CHECK(second.audio.channel("ambient").tracks.at(0).loop_points ==
        AudioLoopPoints{.start = kHalfSecondFrames, .end = 0});
  CHECK(second.audio.channel("rain").volume == kQuarter);
  CHECK(second.audio.channel("sound").tracks.empty());  // Sounds are not state.
}

TEST_CASE("audio: restoring stops sounds and voices at once, and music at the next update") {
  Mixer mixer;
  const std::vector<std::byte> silence = mixer.audio.capture();
  REQUIRE(mixer.audio.music(music_request("bgm/theme")).empty());
  REQUIRE(mixer.audio.sound(sound_request("sfx/door")).empty());
  REQUIRE(mixer.audio.voice("eileen/001").empty());
  REQUIRE(mixer.audio.volume("music", kHalf).empty());

  mixer.audio.restore(silence);

  CHECK(mixer.audio.channel("sound").tracks.empty());
  CHECK_FALSE(mixer.audio.voice_playing());
  CHECK(mixer.audio.channel("music").music.empty());
  CHECK(mixer.audio.channel("music").volume == 1);
  CHECK(mixer.audio.channel("music").tracks.size() == 1);

  mixer.audio.update(0);

  CHECK(mixer.audio.channel("music").tracks.empty());
}

TEST_CASE("audio: restoring from no bytes is silence") {
  Mixer mixer;
  REQUIRE(mixer.audio.music(music_request("bgm/theme")).empty());

  mixer.audio.restore({});
  mixer.audio.update(0);

  CHECK(mixer.audio.capture() == Mixer{}.audio.capture());
  CHECK(mixer.audio.channel("music").tracks.empty());
}

TEST_CASE("audio: a malformed snapshot is rejected and changes nothing") {
  Mixer mixer;
  REQUIRE(mixer.audio.music(music_request("bgm/theme")).empty());
  const std::vector<std::byte> snapshot = mixer.audio.capture();

  std::vector<std::byte> cut = snapshot;
  cut.pop_back();
  CHECK_THROWS_AS(mixer.audio.restore(cut), std::invalid_argument);
  std::vector<std::byte> longer = snapshot;
  longer.push_back(std::byte{0});
  CHECK_THROWS_AS(mixer.audio.restore(longer), std::invalid_argument);
  std::vector<std::byte> other_version = snapshot;
  constexpr std::byte kUnknownVersion{0x7F};
  other_version.front() = kUnknownVersion;
  CHECK_THROWS_AS(mixer.audio.restore(other_version), std::invalid_argument);

  CHECK(mixer.audio.capture() == snapshot);
  CHECK(mixer.audio.channel("music").tracks.size() == 1);
}

TEST_CASE("audio: restoring music whose file is gone throws at the next update") {
  Mixer mixer;
  REQUIRE(mixer.audio.music(music_request("bgm/theme")).empty());
  const std::vector<std::byte> snapshot = mixer.audio.capture();

  const vnmaki::Vfs empty = vnmaki::test::memory_vfs({});
  Audio audio{empty};
  audio.restore(snapshot);

  CHECK_THROWS_AS(audio.update(0), vnmaki::AudioError);
  CHECK(audio.channel("music").tracks.empty());
  CHECK_NOTHROW(audio.update(0));
}

namespace {

constexpr std::string_view kReplayScript = R"(
label("start", function()
  scene("bg/room")
  music("bgm/theme")
  narrate("one")
  sound("sfx/door")
  voice("eileen/001")
  music("bgm/night", { fade = 1 })
  volume("music", 0.5)
  narrate("two")
  music("bgm/theme")
  volume("music", 1)
  narrate("three")
end)
)";

}  // namespace

TEST_CASE("audio: after a replay the music of the target step plays and one-shots did not fire") {
  Stage stage{std::string{kReplayScript}};
  stage.runtime.advance();
  stage.runtime.advance();
  REQUIRE(stage.runtime.position() == vnmaki::ScriptPosition{.label = "start", .step = 3});
  REQUIRE(stage.runtime.label_snapshot() != nullptr);
  const vnmaki::LabelSnapshot snapshot = *stage.runtime.label_snapshot();
  stage.audio.update(1);  // The sound and the voice of step 2 are over.
  stage.recorder.commands.clear();

  stage.runtime.replay(snapshot, {}, 2);
  stage.audio.update(0);

  REQUIRE(stage.runtime.position() == vnmaki::ScriptPosition{.label = "start", .step = 2});
  const AudioChannelState music = stage.audio.channel("music");
  CHECK(music.music == "bgm/night");
  CHECK(music.volume == kHalf);
  REQUIRE(music.tracks.size() == 1);
  CHECK(music.tracks[0].name == "bgm/night");
  CHECK(music.tracks[0].gain == 1);  // No fade: the step is shown as it was.
  CHECK(stage.audio.channel("sound").tracks.empty());
  CHECK_FALSE(stage.audio.voice_playing());
  CHECK(stage.recorder.commands.empty());
  // The scene took part in the same snapshot.
  CHECK(stage.scene.tags(vnmaki::Layer::kBackground) == Lines{"background"});
}

TEST_CASE("audio: music that a replay leaves the same never stops") {
  Stage stage{std::string{kReplayScript}};
  stage.runtime.advance();
  stage.runtime.advance();
  const vnmaki::LabelSnapshot snapshot = *stage.runtime.label_snapshot();
  stage.audio.update(kHalf);
  REQUIRE(stage.audio.channel("music").tracks.at(0).position == kHalfSecondFrames);

  stage.runtime.replay(snapshot, {}, 3);
  stage.audio.update(kQuarter);

  const AudioChannelState music = stage.audio.channel("music");
  REQUIRE(music.tracks.size() == 1);
  CHECK(music.tracks[0].name == "bgm/theme");
  CHECK(music.tracks[0].position == kHalfSecondFrames + kQuarterSecondFrames);
}

TEST_CASE("audio: a replay to step 0 enters the label with the snapshot's music") {
  Stage stage{R"(
label("start", function()
  music("bgm/theme")
  jump("night")
end)
label("night", function()
  narrate("one")
  music("bgm/night")
  voice("eileen/001")
  narrate("two")
end)
)"};
  stage.runtime.advance();
  REQUIRE(stage.audio.channel("music").music == "bgm/night");
  const vnmaki::LabelSnapshot snapshot = *stage.runtime.label_snapshot();

  stage.runtime.replay(snapshot, {}, 0);
  stage.audio.update(0);

  REQUIRE(stage.runtime.position() == vnmaki::ScriptPosition{.label = "night", .step = 1});
  CHECK(stage.audio.channel("music").tracks.at(0).name == "bgm/theme");
  CHECK_FALSE(stage.audio.voice_playing());
}
