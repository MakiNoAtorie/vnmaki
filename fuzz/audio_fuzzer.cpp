// Fuzzes the audio decoders (engine/audio/audio_decoder.hpp): opusfile, vorbisfile, and
// miniaudio's FLAC and WAV readers, plus the engine's conversion to its own format. Audio files
// ship with games and mods, so they are untrusted input.
//
// Besides "does not crash" (the decoders are built with the same sanitizers): bad data is
// reported only as AudioError; a read never gives more frames than asked for; every sample is a
// number from -1 to 1; once a read comes back short the file has ended, and the next read gives
// nothing; loop points from tags have their end after their start; and all of that still holds
// after seeking.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "engine/audio/audio_decoder.hpp"

namespace {

constexpr std::size_t kBlockFrames = 1024;
// Half a second: enough to cross many packets, short enough for thousands of runs.
constexpr std::size_t kMaxFrames = 24'000;

void require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

// Reads up to kMaxFrames and returns how many frames came.
std::size_t read_and_check(vnmaki::AudioDecoder& decoder) {
  std::vector<float> block(kBlockFrames * vnmaki::kAudioChannels);
  std::size_t total = 0;
  while (total < kMaxFrames) {
    const std::size_t frames = decoder.read(block);
    require(frames <= kBlockFrames);
    for (const float sample : std::span{block}.first(frames * vnmaki::kAudioChannels)) {
      require(sample >= -1 && sample <= 1);  // False for NaN too.
    }
    total += frames;
    if (frames < kBlockFrames) {
      require(decoder.read(block) == 0);
      break;
    }
  }
  return total;
}

std::optional<vnmaki::AudioDecoder> open_decoder(std::span<const std::byte> input) {
  try {
    return vnmaki::AudioDecoder{std::vector<std::byte>{input.begin(), input.end()}};
  } catch (const vnmaki::AudioError&) {
    return std::nullopt;
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  std::optional<vnmaki::AudioDecoder> decoder = open_decoder(std::as_bytes(std::span{data, size}));
  if (!decoder) {
    return 0;  // Rejecting bad data is the expected outcome for most inputs.
  }
  if (const std::optional<vnmaki::AudioLoopPoints> loop = decoder->loop_points()) {
    require(loop->end == 0 || loop->end > loop->start);
  }
  const std::size_t frames = read_and_check(*decoder);
  if (decoder->seek(frames / 2)) {
    read_and_check(*decoder);
  }
  if (decoder->seek(0)) {
    read_and_check(*decoder);
  }
  return 0;
}
