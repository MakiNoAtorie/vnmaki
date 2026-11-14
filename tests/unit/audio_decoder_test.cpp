// The audio decoders (docs/SPEC.md section 14), on the clips tools/generators/audio_fixtures.py
// writes: a quarter second of a 440 Hz tone in each format.

#include "engine/audio/audio_decoder.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "tests/support/audio_fixtures.hpp"

namespace {

using vnmaki::AudioDecoder;
using vnmaki::AudioError;
using vnmaki::AudioLoopPoints;
using vnmaki::kAudioChannels;
using vnmaki::test::audio_fixture_bytes;

// A quarter of a second at kAudioSampleRate.
constexpr std::size_t kToneFrames = 12'000;
constexpr std::size_t kBlockFrames = 1'000;

// The root mean square of a sine is its amplitude over the square root of two. The generated WAV
// files have amplitude 1/2, the pre-encoded clips 1/8.
constexpr double kWavRms = 0.353553;
constexpr double kEncodedRms = 0.088388;
constexpr double kLosslessTolerance = 0.01;
constexpr double kLossyTolerance = 0.15;
constexpr double kSilence = 0.001;

struct Decoded {
  std::size_t frames = 0;
  double left_rms = 0;
  double right_rms = 0;
};

// Reads the decoder to its end, a block at a time.
Decoded decode_rest(AudioDecoder& decoder) {
  Decoded result;
  double left = 0;
  double right = 0;
  std::vector<float> block(kBlockFrames * kAudioChannels);
  for (;;) {
    const std::size_t frames = decoder.read(block);
    REQUIRE(frames <= kBlockFrames);
    for (std::size_t i = 0; i < frames; ++i) {
      const double l = block[i * kAudioChannels];
      const double r = block[(i * kAudioChannels) + 1];
      REQUIRE(std::abs(l) <= 1);
      REQUIRE(std::abs(r) <= 1);
      left += l * l;
      right += r * r;
    }
    result.frames += frames;
    if (frames < kBlockFrames) {
      break;
    }
  }
  if (result.frames > 0) {
    result.left_rms = std::sqrt(left / static_cast<double>(result.frames));
    result.right_rms = std::sqrt(right / static_cast<double>(result.frames));
  }
  return result;
}

Decoded decode(const std::string& name) {
  AudioDecoder decoder{audio_fixture_bytes(name)};
  return decode_rest(decoder);
}

}  // namespace

TEST_CASE("audio decoder: a mono WAV file plays on both channels") {
  const Decoded tone = decode("tone.wav");

  CHECK(tone.frames == kToneFrames);
  CHECK(tone.left_rms == doctest::Approx(kWavRms).epsilon(kLosslessTolerance));
  CHECK(tone.right_rms == doctest::Approx(kWavRms).epsilon(kLosslessTolerance));
}

TEST_CASE("audio decoder: a 44.1 kHz stereo WAV file is resampled and keeps its channels") {
  // The right channel of this file is silent.
  const Decoded tone = decode("tone44.wav");

  CHECK(tone.frames == kToneFrames);
  CHECK(tone.left_rms == doctest::Approx(kWavRms).epsilon(kLosslessTolerance));
  CHECK(tone.right_rms < kSilence);
}

TEST_CASE("audio decoder: a FLAC file decodes") {
  const Decoded tone = decode("tone.flac");

  CHECK(tone.frames == kToneFrames);
  CHECK(tone.left_rms == doctest::Approx(kEncodedRms).epsilon(kLossyTolerance));
  CHECK(tone.right_rms == doctest::Approx(tone.left_rms));
}

TEST_CASE("audio decoder: an Opus file decodes") {
  const Decoded tone = decode("tone.opus");

  CHECK(tone.frames == kToneFrames);
  CHECK(tone.left_rms == doctest::Approx(kEncodedRms).epsilon(kLossyTolerance));
  CHECK(tone.right_rms == doctest::Approx(tone.left_rms));
}

TEST_CASE("audio decoder: a Vorbis file decodes") {
  const Decoded tone = decode("tone.ogg");

  CHECK(tone.frames == kToneFrames);
  CHECK(tone.left_rms == doctest::Approx(kEncodedRms).epsilon(kLossyTolerance));
  CHECK(tone.right_rms == doctest::Approx(tone.left_rms));
}

TEST_CASE("audio decoder: every format seeks") {
  constexpr std::uint64_t kSeekTo = 9'000;
  constexpr std::size_t kRest = 3'000;
  for (const std::string name : {"tone.wav", "tone44.wav", "tone.flac", "tone.opus", "tone.ogg"}) {
    CAPTURE(name);
    AudioDecoder decoder{audio_fixture_bytes(name)};
    CHECK(decode_rest(decoder).frames == kToneFrames);

    // A resampled file seeks to the frame of the file at or before the one asked for.
    REQUIRE(decoder.seek(kSeekTo));
    const std::size_t rest = decode_rest(decoder).frames;
    CHECK(rest >= kRest);
    CHECK(rest <= kRest + 1);

    REQUIRE(decoder.seek(0));
    CHECK(decode_rest(decoder).frames == kToneFrames);
  }
}

TEST_CASE("audio decoder: loop points come from the LOOPSTART, LOOPLENGTH and LOOPEND tags") {
  // loop.opus has LOOPSTART=4800 and LOOPLENGTH=4800, loop.ogg LOOPSTART=2400 and LOOPEND=9600.
  CHECK(AudioDecoder{audio_fixture_bytes("loop.opus")}.loop_points() ==
        AudioLoopPoints{.start = 4800, .end = 9600});
  CHECK(AudioDecoder{audio_fixture_bytes("loop.ogg")}.loop_points() ==
        AudioLoopPoints{.start = 2400, .end = 9600});
  CHECK(AudioDecoder{audio_fixture_bytes("tone.ogg")}.loop_points() == std::nullopt);
  CHECK(AudioDecoder{audio_fixture_bytes("tone.wav")}.loop_points() == std::nullopt);
}

TEST_CASE("audio decoder: data that is not audio is an AudioError") {
  CHECK_THROWS_AS(AudioDecoder{std::vector<std::byte>{}}, AudioError);

  const std::string prose = "OggS is how an Ogg page starts, but this is prose.";
  std::vector<std::byte> text;
  text.reserve(prose.size());
  for (const char c : prose) {
    text.push_back(static_cast<std::byte>(c));
  }
  CHECK_THROWS_AS(AudioDecoder{text}, AudioError);
}

TEST_CASE("audio decoder: a file cut short plays what is there, or is an AudioError") {
  const auto cut_short = [](const std::string& name) {
    std::vector<std::byte> bytes = audio_fixture_bytes(name);
    bytes.resize(bytes.size() - (bytes.size() / 4));
    return bytes;
  };
  for (const std::string name : {"tone.wav", "tone.flac"}) {
    CAPTURE(name);
    AudioDecoder decoder{cut_short(name)};

    const Decoded tone = decode_rest(decoder);
    CHECK(tone.frames > 0);
    CHECK(tone.frames < kToneFrames);
  }
  // These clips are one Ogg page of audio each, so nothing of it is left.
  CHECK_THROWS_AS(AudioDecoder{cut_short("tone.opus")}, AudioError);
  CHECK_THROWS_AS(AudioDecoder{cut_short("tone.ogg")}, AudioError);
}
