// Video (docs/SPEC.md section 14): the WebM reader, the AV1 decoder, the player and its clocks, the
// `movie` call and the movie displayable. The clip is the committed fixture
// tests/fixtures/video/clip.webm: 64 x 48, 10 pictures a second, two red, two green and two blue
// pictures (0.6 s), and a tone. No GPU: a recording renderer takes the textures.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/audio/audio.hpp"
#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/image.hpp"
#include "engine/runtime/headless_runner.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/movie_displayable.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"
#include "engine/video/movie.hpp"
#include "engine/video/movie_player.hpp"
#include "engine/video/movie_presenter.hpp"
#include "engine/video/video_decoder.hpp"
#include "tests/support/memory_vfs.hpp"
#include "tests/support/recording_renderer.hpp"

namespace {

using vnmaki::Audio;
using vnmaki::Image;
using vnmaki::JobSystem;
using vnmaki::MovieData;
using vnmaki::MovieDisplayable;
using vnmaki::MovieError;
using vnmaki::MoviePlayer;
using vnmaki::MoviePresenter;
using vnmaki::MovieRequest;
using vnmaki::RunState;
using vnmaki::Scene;
using vnmaki::VideoDecoder;
using vnmaki::test::RecordingRenderer;
using Lines = std::vector<std::string>;

struct Rgb {
  int r = 0;
  int g = 0;
  int b = 0;
};

// What the encoder made of (255, 0, 0), (0, 255, 0) and (0, 0, 255): see the fixture's README.
constexpr Rgb kRed{.r = 251, .g = 0, .b = 0};
constexpr Rgb kGreen{.r = 0, .g = 252, .b = 0};
constexpr Rgb kBlue{.r = 0, .g = 0, .b = 252};
constexpr int kTolerance = 16;

constexpr std::size_t kFrameCount = 6;
constexpr std::array<Rgb, kFrameCount> kFrameColors{kRed, kRed, kGreen, kGreen, kBlue, kBlue};
constexpr double kFrameSeconds = 0.1;
constexpr double kClipSeconds = 0.6;
constexpr double kTimeEpsilon = 1e-6;
constexpr vnmaki::Size kClipSize{.width = 64, .height = 48};
constexpr vnmaki::Size kVirtualSize{.width = 320, .height = 240};

constexpr double kInFirstFrame = 0.05;
constexpr double kInSecondColor = 0.25;
constexpr double kInThirdColor = 0.45;
constexpr double kNearEnd = 0.55;
constexpr double kPastEnd = 1.0;
constexpr double kAudioStep = 0.05;
constexpr float kHeroX = 0.25F;  // The x of "hero" in its script.
constexpr double kWrapStep = 0.2;
constexpr double kClockTolerance = 0.02;
constexpr std::size_t kAudioSteps = 5;
constexpr std::size_t kMutations = 120;
constexpr std::uint32_t kSeed = 25;
constexpr std::size_t kMaxFlips = 4;

constexpr std::chrono::seconds kTimeout{10};
constexpr std::chrono::milliseconds kPoll{1};

constexpr std::string_view kClipPath = "movies/clip.webm";

std::string read_clip() {
  std::ifstream stream{std::string{VNMAKI_VIDEO_FIXTURES_DIR} + "/clip.webm", std::ios::binary};
  REQUIRE(stream.good());
  return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

std::vector<std::byte> clip_bytes() {
  const std::string text = read_clip();
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const char c : text) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  return bytes;
}

// A game with a script and the clip.
vnmaki::Vfs game_files(const std::string& script) {
  return vnmaki::test::memory_vfs({{"scripts/main.lua", script},
                                   {std::string{kClipPath}, read_clip()},
                                   {"movies/bad.webm", "this is not a movie"}});
}

bool near_color(const Image& image, const Rgb& expected) {
  const auto at = [&image](int x, int y) {
    const std::size_t i =
        ((static_cast<std::size_t>(y) * static_cast<std::size_t>(image.size.width)) +
         static_cast<std::size_t>(x)) *
        4;
    return Rgb{.r = image.pixels[i], .g = image.pixels[i + 1], .b = image.pixels[i + 2]};
  };
  const auto close = [&expected](const Rgb& got) {
    return std::abs(got.r - expected.r) <= kTolerance &&
           std::abs(got.g - expected.g) <= kTolerance && std::abs(got.b - expected.b) <= kTolerance;
  };
  return close(at(0, 0)) && close(at(image.size.width / 2, image.size.height / 2)) &&
         close(at(image.size.width - 1, image.size.height - 1));
}

// Calls `step` until `done` is true, or fails after kTimeout. The decoder runs on a job, so a
// picture needs a moment.
template <typename Step, typename Done>
void settle(const Step& step, const Done& done) {
  const auto deadline = std::chrono::steady_clock::now() + kTimeout;
  while (true) {
    step();
    if (done()) {
      return;
    }
    REQUIRE_MESSAGE(std::chrono::steady_clock::now() < deadline, "timed out");
    std::this_thread::sleep_for(kPoll);
  }
}

std::shared_ptr<const MovieData> clip() {
  const vnmaki::Vfs vfs = game_files("");
  return vnmaki::load_movie(vfs, kClipPath);
}

// Moves a player's clock by `seconds`, then lets the decoder catch up until the picture is
// `expected`.
void play_to(MoviePlayer& player, double seconds, const Rgb& expected) {
  player.update(seconds);
  settle([&player] { player.update(0); },
         [&] { return player.picture() != nullptr && near_color(*player.picture(), expected); });
}

}  // namespace

TEST_CASE("video: the container gives the size, the times, the duration and the audio") {
  const MovieData movie = vnmaki::open_movie(clip_bytes());

  CHECK(movie.size == kClipSize);
  REQUIRE(movie.video.size() == kFrameCount);
  for (std::size_t i = 0; i < kFrameCount; ++i) {
    CHECK(movie.video[i].time == doctest::Approx(static_cast<double>(i) * kFrameSeconds));
  }
  CHECK(movie.duration == doctest::Approx(kClipSeconds));
  REQUIRE(movie.audio.size() > 4);
  CHECK(std::to_integer<char>(movie.audio[0]) == 'O');  // An Ogg file for the audio decoder.
  CHECK(std::to_integer<char>(movie.audio[1]) == 'g');
}

TEST_CASE("video: the decoder gives each picture in order, with the color of its frame") {
  VideoDecoder decoder{clip()};

  std::vector<vnmaki::MovieFrame> frames = decoder.decode(kFrameCount);
  while (!decoder.at_end()) {
    for (vnmaki::MovieFrame& frame : decoder.decode(kFrameCount)) {
      frames.push_back(std::move(frame));
    }
  }

  REQUIRE(frames.size() == kFrameCount);
  for (std::size_t i = 0; i < kFrameCount; ++i) {
    CAPTURE(i);
    CHECK(frames[i].time == doctest::Approx(static_cast<double>(i) * kFrameSeconds));
    CHECK(frames[i].image.size == kClipSize);
    CHECK(near_color(frames[i].image, kFrameColors[i]));
  }

  decoder.rewind();
  CHECK_FALSE(decoder.at_end());
  const std::vector<vnmaki::MovieFrame> again = decoder.decode(1);
  REQUIRE(again.size() == 1);
  CHECK(near_color(again.front().image, kRed));
}

TEST_CASE("video: bad files are errors, never crashes") {
  CHECK_THROWS_AS(static_cast<void>(vnmaki::open_movie({})), MovieError);
  const std::string text = "this is not a movie";
  std::vector<std::byte> not_webm;
  for (const char c : text) {
    not_webm.push_back(static_cast<std::byte>(c));
  }
  CHECK_THROWS_AS(static_cast<void>(vnmaki::open_movie(not_webm)), MovieError);

  // Cuts and flipped bytes of a good clip either play or are a MovieError.
  const std::vector<std::byte> good = clip_bytes();
  // Seeded with a constant: a mutation that fails must come again.
  std::mt19937 random{kSeed};  // NOLINT(bugprone-random-generator-seed)
  std::size_t errors = 0;
  for (std::size_t i = 0; i < kMutations; ++i) {
    std::vector<std::byte> bad = good;
    if (i % 2 == 0) {
      bad.resize(std::uniform_int_distribution<std::size_t>{0, good.size() - 1}(random));
    }
    const std::size_t flips = std::uniform_int_distribution<std::size_t>{0, kMaxFlips}(random);
    for (std::size_t f = 0; f < flips && !bad.empty(); ++f) {
      const std::size_t at = std::uniform_int_distribution<std::size_t>{0, bad.size() - 1}(random);
      bad[at] = static_cast<std::byte>(std::uniform_int_distribution<int>{0, UINT8_MAX}(random));
    }
    try {
      auto movie = std::make_shared<const MovieData>(vnmaki::open_movie(bad));
      VideoDecoder decoder{movie};
      while (!decoder.at_end()) {
        static_cast<void>(decoder.decode(kFrameCount));
      }
    } catch (const MovieError&) {
      ++errors;
    }
  }
  CHECK(errors > 0);
}

TEST_CASE("video: a missing movie and a bad one are reported with their path") {
  const vnmaki::Vfs vfs = game_files("");

  CHECK(vnmaki::movie_problem(vfs, "movies/none.webm") == "movie 'movies/none.webm' not found");
  CHECK(vnmaki::movie_problem(vfs, "movies/bad.webm") ==
        "movie 'movies/bad.webm' is not a WebM file");
  CHECK(vnmaki::movie_problem(vfs, kClipPath).empty());
}

TEST_CASE("video: without audio the picture follows the loop clock") {
  JobSystem jobs;
  MoviePlayer player{clip(), jobs, nullptr, false};

  // The clock does not start before the first picture is there.
  settle([&player] { player.update(0); }, [&player] { return player.ready(); });
  CHECK(player.time() == 0);
  CHECK(near_color(*player.picture(), kRed));
  CHECK(player.size() == kClipSize);

  play_to(player, kInFirstFrame, kRed);
  CHECK(player.time() == doctest::Approx(kInFirstFrame));
  play_to(player, kInSecondColor - kInFirstFrame, kGreen);
  CHECK(player.time() == doctest::Approx(kInSecondColor));
  play_to(player, kInThirdColor - kInSecondColor, kBlue);
  play_to(player, kNearEnd - kInThirdColor, kBlue);
  CHECK_FALSE(player.finished());

  settle([&player] { player.update(kPastEnd); }, [&player] { return player.finished(); });
  CHECK(player.time() >= kClipSeconds - kTimeEpsilon);
}

TEST_CASE("video: a loop starts again and never finishes") {
  JobSystem jobs;
  MoviePlayer player{clip(), jobs, nullptr, true};

  play_to(player, 0, kRed);
  play_to(player, kInThirdColor, kBlue);
  // 0.45 + 0.2 is 0.05 into the second pass.
  play_to(player, kWrapStep, kRed);
  CHECK(player.time() == doctest::Approx(kInThirdColor + kWrapStep - kClipSeconds));
  play_to(player, kInSecondColor - kInFirstFrame, kGreen);
  for (int pass = 0; pass < 3; ++pass) {
    player.update(kClipSeconds);
  }
  CHECK_FALSE(player.finished());
}

TEST_CASE("video: with audio the picture follows the audio clock") {
  const vnmaki::Vfs vfs = game_files("");
  Audio audio{vfs};  // No device: update() mixes by hand.
  JobSystem jobs;
  MoviePlayer player{clip(), jobs, &audio, false};

  settle([&player] { player.update(0); }, [&player] { return player.ready(); });
  REQUIRE(audio.channel("movie").tracks.size() == 1);
  CHECK(audio.channel("movie").tracks.front().position == 0);

  // The player is given no seconds at all: only the audio moves its clock.
  for (std::size_t i = 0; i < kAudioSteps; ++i) {
    audio.update(kAudioStep);
    player.update(0);
  }
  CHECK(player.time() == doctest::Approx(kAudioStep * kAudioSteps).epsilon(kClockTolerance));
  settle([&player] { player.update(0); },
         [&player] { return near_color(*player.picture(), kGreen); });

  // The audio ends with the movie. The loop clock takes over, and the movie finishes.
  audio.update(kPastEnd);
  settle([&player] { player.update(0); },
         [&audio] { return audio.channel("movie").tracks.empty(); });
  settle([&player] { player.update(kPastEnd); }, [&player] { return player.finished(); });
  CHECK(audio.channel("movie").tracks.empty());
}

TEST_CASE("video: the movie's audio stops with the player, and a restore stops it too") {
  const vnmaki::Vfs vfs = game_files("");
  Audio audio{vfs};
  JobSystem jobs;
  {
    MoviePlayer player{clip(), jobs, &audio, true};
    settle([&player] { player.update(0); }, [&player] { return player.ready(); });
    CHECK(audio.channel("movie").tracks.size() == 1);
  }
  CHECK(audio.channel("movie").tracks.empty());

  MoviePlayer looping{clip(), jobs, &audio, true};
  settle([&looping] { looping.update(0); }, [&looping] { return looping.ready(); });
  REQUIRE(audio.channel("movie").tracks.size() == 1);
  audio.restore({});
  audio.update(0);
  CHECK(audio.channel("movie").tracks.empty());
}

TEST_CASE("video: the picture follows a null audio device") {
  if (!Audio::device_supported()) {
    return;
  }
  const vnmaki::Vfs vfs = game_files("");
  std::unique_ptr<Audio> audio;
  try {
    audio = std::make_unique<Audio>(
        vfs, vnmaki::AudioOptions{.output = vnmaki::AudioOutput::kNullDevice});
  } catch (const vnmaki::AudioError&) {
    return;  // The machine cannot even start a null device.
  }
  JobSystem jobs;
  MoviePlayer player{clip(), jobs, audio.get(), false};

  // No seconds are given, so the clock moves only if the device thread plays the audio.
  settle([&player] { player.update(0); }, [&player] { return player.time() > kInSecondColor; });
  const double time = player.time();
  const auto index = std::min(static_cast<std::size_t>(time / kFrameSeconds), kFrameCount - 1);
  const Rgb behind = kFrameColors[index == 0 ? 0 : index - 1];
  // The picture is the one of this moment, or at most one behind it.
  CHECK((near_color(*player.picture(), kFrameColors[index]) ||
         near_color(*player.picture(), behind)));

  // Once the audio is over the loop clock takes the picture to the end.
  settle([&player] { player.update(kAudioStep); }, [&player] { return player.finished(); });
  CHECK(player.time() >= kClipSeconds - kTimeEpsilon);
}

namespace {

// A script started at its `start` label, with a scene and a movie presenter.
struct Stage {
  explicit Stage(const std::string& script)
      : vfs(game_files(script)),
        scene(vfs, kVirtualSize),
        runtime(vfs, options()),
        movies(runtime, vfs, jobs) {
    runtime.load_scripts();
    runtime.start("start");
  }

  [[nodiscard]] vnmaki::ScriptRuntimeOptions options() {
    vnmaki::ScriptRuntimeOptions result;
    result.scene_state = &scene;
    result.scene = &scene;
    return result;
  }

  [[nodiscard]] bool at_movie() const {
    const vnmaki::BlockingRequest* request = runtime.request();
    return request != nullptr && std::holds_alternative<MovieRequest>(*request);
  }

  vnmaki::Vfs vfs;
  Scene scene;
  vnmaki::ScriptRuntime runtime;
  JobSystem jobs;
  MoviePresenter movies;
};

constexpr std::string_view kStory = R"(
label("start", function()
  narrate("before")
  movie("movies/clip.webm", { skippable = SKIPPABLE })
  narrate("after")
  narrate("last")
end)
)";

std::string story(bool skippable) {
  std::string text{kStory};
  text.replace(text.find("SKIPPABLE"), std::string_view{"SKIPPABLE"}.size(),
               skippable ? "true" : "false");
  return text;
}

}  // namespace

TEST_CASE("video: movie() blocks like a wait until the movie has played") {
  Stage stage{story(false)};
  REQUIRE(stage.runtime.state() == RunState::kWaiting);
  stage.runtime.advance();
  REQUIRE(stage.at_movie());
  CHECK(std::get<MovieRequest>(*stage.runtime.request()).path == kClipPath);
  CHECK_FALSE(std::get<MovieRequest>(*stage.runtime.request()).skippable);
  CHECK(stage.movies.waiting());
  CHECK(stage.runtime.position().wait);  // A rollback goes back over it.

  // It plays for its 0.6 s. Time is given to the presenter, not taken from a clock.
  settle([&stage] { stage.movies.update(kFrameSeconds); }, [&stage] { return !stage.at_movie(); });
  REQUIRE(stage.runtime.state() == RunState::kWaiting);
  CHECK(std::get<vnmaki::NarrateRequest>(*stage.runtime.request()).text == "after");
}

TEST_CASE("video: a click ends a skippable movie and skip mode ends any movie") {
  Stage skippable{story(true)};
  skippable.runtime.advance();
  REQUIRE(skippable.at_movie());
  CHECK(skippable.movies.click());
  CHECK_FALSE(skippable.at_movie());
  CHECK(std::get<vnmaki::NarrateRequest>(*skippable.runtime.request()).text == "after");
  CHECK_FALSE(skippable.movies.click());  // Not at a movie any more: the click is not used.

  Stage fixed{story(false)};
  fixed.runtime.advance();
  REQUIRE(fixed.at_movie());
  CHECK(fixed.movies.click());  // Used, but the movie plays on.
  CHECK(fixed.at_movie());
  CHECK(fixed.movies.skip());
  CHECK_FALSE(fixed.at_movie());
}

TEST_CASE("video: a replay does not play the movie") {
  Stage stage{story(false)};
  stage.runtime.advance();
  REQUIRE(stage.at_movie());
  stage.movies.skip();
  stage.runtime.advance();  // "after" -> "last"
  REQUIRE(stage.runtime.state() == RunState::kWaiting);

  // Rolling back replays the label from its start to the line "after", and passes the movie.
  REQUIRE(stage.runtime.roll_back());
  CHECK_FALSE(stage.at_movie());
  CHECK_FALSE(stage.movies.waiting());
  CHECK(std::get<vnmaki::NarrateRequest>(*stage.runtime.request()).text == "after");
  // Going back to "before" and forward again plays it, because that is live play.
  REQUIRE(stage.runtime.roll_back());
  stage.runtime.advance();
  CHECK(stage.at_movie());
}

TEST_CASE("video: the headless runner does not play movies") {
  const vnmaki::Vfs vfs = game_files(story(false));
  const std::vector<vnmaki::ScriptInput> inputs(1, vnmaki::AdvanceInput{});
  const vnmaki::HeadlessResult result = vnmaki::run_headless(vfs, inputs);

  CHECK(result.transcript ==
        Lines{"label start", "narrate before", "movie movies/clip.webm", "narrate after"});
}

TEST_CASE("video: a bad or missing movie is an error at the script line") {
  const auto error_of = [](const std::string& call) {
    const Stage stage{"label(\"start\", function()\n" + call + "\nend)\n"};
    REQUIRE(stage.runtime.state() == RunState::kFailed);
    return stage.runtime.error();
  };

  CHECK(error_of("movie(\"movies/none.webm\")") ==
        "scripts/main.lua:2: movie 'movies/none.webm' not found");
  CHECK(error_of("movie(\"movies/bad.webm\")") ==
        "scripts/main.lua:2: movie 'movies/bad.webm' is not a WebM file");
  CHECK(error_of("show(\"m\", { movie = \"movies/bad.webm\" })") ==
        "scripts/main.lua:2: movie 'movies/bad.webm' is not a WebM file");
  CHECK(error_of("show(\"m\", { movie = \"movies/none.webm\" })") ==
        "scripts/main.lua:2: movie 'movies/none.webm' not found");
  CHECK(error_of("movie(3)") ==
        "scripts/main.lua:2: bad argument #1 to 'movie' (string expected, got number)");
  CHECK(error_of("movie(\"movies/clip.webm\", { skippable = 1 })") ==
        "scripts/main.lua:2: bad option 'skippable' to 'movie' (boolean expected, got number)");
}

namespace {

constexpr std::string_view kShowStory = R"(
label("start", function()
  show("rain", { movie = "movies/clip.webm", loop = true })
  show("intro", { movie = "movies/clip.webm", layer = "overlay", x = 0.5, y = 0.5 })
  narrate("shown")
end)
)";

// Runs the scene's prepare and update, with no time passing, until `done`.
template <typename Done>
void pump(Stage& stage, RecordingRenderer& renderer, const Done& done) {
  settle(
      [&] {
        stage.scene.prepare(renderer, stage.jobs);
        stage.scene.update(0);
      },
      done);
}

// The player of the movie displayable `tag`, or null while the movie loads.
const MoviePlayer* find_player(Stage& stage, std::string_view tag) {
  const auto* movie = dynamic_cast<const MovieDisplayable*>(stage.scene.find(tag));
  REQUIRE(movie != nullptr);
  return movie->player();
}

// True when the movie `tag` has its first picture, uploaded.
bool shows_picture(Stage& stage, std::string_view tag) {
  const MoviePlayer* player = find_player(stage, tag);
  return player != nullptr && player->ready() && player->texture().has_value();
}

const MoviePlayer& player_of(Stage& stage, std::string_view tag) {
  const MoviePlayer* player = find_player(stage, tag);
  REQUIRE(player != nullptr);
  return *player;
}

}  // namespace

TEST_CASE("video: show with a movie makes a displayable that draws the picture") {
  Stage stage{std::string{kShowStory}};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  const auto* rain = dynamic_cast<const MovieDisplayable*>(stage.scene.find("rain"));
  const auto* intro = dynamic_cast<const MovieDisplayable*>(stage.scene.find("intro"));
  REQUIRE(rain != nullptr);
  REQUIRE(intro != nullptr);
  CHECK(rain->loop());
  CHECK_FALSE(intro->loop());
  CHECK(rain->path() == kClipPath);
  CHECK(stage.scene.layer_of("rain") == vnmaki::Layer::kCharacters);
  CHECK(stage.scene.layer_of("intro") == vnmaki::Layer::kOverlay);

  RecordingRenderer renderer;
  pump(stage, renderer,
       [&] { return shows_picture(stage, "rain") && shows_picture(stage, "intro"); });
  stage.scene.draw(renderer);

  // Both movies draw their first picture: the whole 64 x 48 clip, bottom-centered for `rain`.
  REQUIRE(renderer.draws.size() == 2);
  const vnmaki::RectF box = rain->bounds();
  CHECK(box.width == doctest::Approx(static_cast<float>(kClipSize.width)));
  CHECK(box.height == doctest::Approx(static_cast<float>(kClipSize.height)));
  CHECK(box.y + box.height == doctest::Approx(static_cast<float>(kVirtualSize.height)));
  CHECK(near_color(*rain->player()->picture(), kRed));
}

TEST_CASE("video: a movie that does not loop is what wait waits for") {
  Stage stage{std::string{kShowStory}};
  RecordingRenderer renderer;
  CHECK_FALSE(stage.scene.tweening("rain"));  // It loops.
  CHECK(stage.scene.tweening("intro"));
  CHECK(stage.scene.tweening(""));

  pump(stage, renderer, [&] { return shows_picture(stage, "intro"); });
  settle(
      [&] {
        stage.scene.prepare(renderer, stage.jobs);
        stage.scene.update(kPastEnd);
      },
      [&] { return !stage.scene.tweening("intro"); });
  CHECK_FALSE(stage.scene.tweening(""));

  // A click or skip puts it at its end at once, also before its file has loaded.
  Stage other{std::string{kShowStory}};
  other.scene.finish_tweens("intro");
  CHECK_FALSE(other.scene.tweening("intro"));
}

TEST_CASE("video: after a load a movie plays again from time 0, in its place") {
  Stage stage{R"(
label("start", function()
  show("back", { movie = "movies/clip.webm", loop = true, layer = "background" })
  show("hero", { movie = "movies/clip.webm", x = 0.25 })
  narrate("shown")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  RecordingRenderer renderer;
  pump(stage, renderer, [&] { return shows_picture(stage, "hero"); });
  stage.scene.update(kInThirdColor);
  settle([&] { stage.scene.update(0); },
         [&] { return near_color(*player_of(stage, "hero").picture(), kBlue); });

  const std::vector<std::byte> snapshot = stage.scene.capture();
  stage.scene.restore({});
  CHECK(stage.scene.find("hero") == nullptr);
  stage.scene.restore(snapshot);

  const auto* hero = dynamic_cast<const MovieDisplayable*>(stage.scene.find("hero"));
  const auto* back = dynamic_cast<const MovieDisplayable*>(stage.scene.find("back"));
  REQUIRE(hero != nullptr);
  REQUIRE(back != nullptr);
  CHECK(hero->player() == nullptr);  // Not loaded yet, so it starts at time 0.
  CHECK(back->loop());
  CHECK_FALSE(hero->loop());
  CHECK(hero->properties().position.x ==
        doctest::Approx(kHeroX * static_cast<float>(kVirtualSize.width)));
  CHECK(stage.scene.layer_of("back") == vnmaki::Layer::kBackground);

  pump(stage, renderer, [&] { return shows_picture(stage, "hero"); });
  CHECK(player_of(stage, "hero").time() == 0);
  CHECK(near_color(*player_of(stage, "hero").picture(), kRed));
}

TEST_CASE("video: a movie among other displayables keeps its place in a snapshot") {
  const vnmaki::Vfs vfs =
      vnmaki::test::memory_vfs({{"images/dot.png", ""}, {std::string{kClipPath}, read_clip()}});
  Scene scene{vfs, kVirtualSize};
  const auto show = [&scene](const std::string& tag, const std::string& movie) {
    vnmaki::ShowRequest request{.tag = tag, .image = "dot", .layer = "overlay"};
    if (!movie.empty()) {
      request.movie = movie;
    }
    REQUIRE(scene.show(request).empty());
  };
  show("a", "");
  show("m", std::string{kClipPath});
  show("b", "");
  const std::vector<std::byte> snapshot = scene.capture();
  scene.restore({});
  scene.restore(snapshot);

  CHECK(scene.tags(vnmaki::Layer::kOverlay) == std::vector<std::string>{"a", "m", "b"});
  CHECK(dynamic_cast<const MovieDisplayable*>(scene.find("m")) != nullptr);
}

TEST_CASE("video: a movie displayable plays its audio on the movie channel until it is hidden") {
  const vnmaki::Vfs vfs = game_files(R"(
label("start", function()
  show("rain", { movie = "movies/clip.webm", loop = true })
  narrate("shown")
  hide("rain")
  narrate("hidden")
end)
)");
  Audio audio{vfs};
  Scene scene{vfs, kVirtualSize};
  scene.set_movie_audio(&audio);
  vnmaki::ScriptRuntimeOptions options;
  options.scene = &scene;
  options.scene_state = &scene;
  vnmaki::ScriptRuntime runtime{vfs, options};
  runtime.load_scripts();
  runtime.start("start");
  REQUIRE_MESSAGE(runtime.state() == RunState::kWaiting, runtime.error());
  JobSystem jobs;
  RecordingRenderer renderer;

  settle(
      [&] {
        scene.prepare(renderer, jobs);
        scene.update(0);
      },
      [&] { return audio.channel("movie").tracks.size() == 1; });
  CHECK(audio.channel("movie").tracks.front().loop);

  runtime.advance();  // hide
  CHECK(audio.channel("movie").tracks.empty());
}

namespace {

// A clip that opens but whose pictures do not decode: the clip with its second half scrambled
// (seeded, so the same bytes every run).
std::string undecodable_clip() {
  std::string bytes = read_clip();
  std::mt19937 random{kSeed};  // NOLINT(bugprone-random-generator-seed)
  for (std::size_t attempt = 0; attempt < kMutations; ++attempt) {
    std::string bad = bytes;
    for (std::size_t f = 0; f < kMaxFlips + attempt; ++f) {
      const std::size_t at =
          std::uniform_int_distribution<std::size_t>{bad.size() / 2, bad.size() - 1}(random);
      bad[at] = static_cast<char>(std::uniform_int_distribution<int>{0, UINT8_MAX}(random));
    }
    std::vector<std::byte> raw;
    for (const char c : bad) {
      raw.push_back(static_cast<std::byte>(c));
    }
    try {
      auto movie = std::make_shared<const MovieData>(vnmaki::open_movie(raw));
      VideoDecoder decoder{movie};
      while (!decoder.at_end()) {
        static_cast<void>(decoder.decode(kFrameCount));
      }
    } catch (const MovieError&) {
      return bad;
    }
  }
  FAIL("no undecodable clip found");
  return {};
}

}  // namespace

TEST_CASE("video: a movie that fails to decode is skipped with a warning, not thrown") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs(
      {{"scripts/main.lua", story(false)}, {std::string{kClipPath}, undecodable_clip()}});
  vnmaki::ScriptRuntime runtime{vfs, {}};
  runtime.load_scripts();
  runtime.start("start");
  runtime.advance();  // past "before"
  JobSystem jobs;
  MoviePresenter movies{runtime, vfs, jobs};
  Lines warnings;
  movies.set_warning_handler([&](std::string_view message) { warnings.emplace_back(message); });
  REQUIRE(movies.waiting());

  settle([&] { movies.update(kPastEnd); }, [&] { return !movies.waiting(); });
  CHECK(warnings.size() == 1);
  CHECK(runtime.state() == RunState::kWaiting);  // the story went on to "after"
}
