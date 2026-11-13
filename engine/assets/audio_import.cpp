#include "engine/assets/audio_import.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include <opusenc.h>

#include "engine/audio/audio_decoder.hpp"

namespace vnmaki {

namespace {

constexpr double kDecibelsPerDecade = 20;
constexpr double kDecadeBase = 10;
// 20 ms: the frame size Opus likes, and a size that is small for the stack.
constexpr std::size_t kBlockFrames = 960;
constexpr int kEncoderFamily = 0;  // mono or stereo

struct Levels {
  double peak = 0;
  double rms = 0;
};

Levels measure(AudioDecoder& decoder) {
  std::array<float, kBlockFrames * kAudioChannels> block{};
  double peak = 0;
  double energy = 0;
  std::uint64_t samples = 0;
  for (std::size_t frames = decoder.read(block); frames > 0; frames = decoder.read(block)) {
    for (const float sample : std::span{block}.first(frames * kAudioChannels)) {
      peak = std::max(peak, static_cast<double>(std::abs(sample)));
      energy += static_cast<double>(sample) * static_cast<double>(sample);
    }
    samples += frames * kAudioChannels;
  }
  return {.peak = peak, .rms = samples == 0 ? 0 : std::sqrt(energy / static_cast<double>(samples))};
}

int write_bytes(void* user_data, const unsigned char* data, opus_int32 length) {
  auto* out = static_cast<std::vector<std::byte>*>(user_data);
  for (opus_int32 i = 0; i < length; ++i) {
    out->push_back(static_cast<std::byte>(data[i]));
  }
  return 0;
}

int close_stream(void* /*user_data*/) { return 0; }

struct CommentsDeleter {
  void operator()(OggOpusComments* comments) const { ope_comments_destroy(comments); }
};
struct EncoderDeleter {
  void operator()(OggOpusEnc* encoder) const { ope_encoder_destroy(encoder); }
};

[[noreturn]] void fail(const char* what, int code) {
  throw std::runtime_error{std::string{"cannot encode Opus: "} + what + ": " + ope_strerror(code)};
}

}  // namespace

double normalization_gain(double peak, double rms, const ImportSettings& settings) {
  if (settings.normalize == AudioNormalize::kNone || peak <= 0) {
    return 1;
  }
  const double target = std::pow(kDecadeBase, settings.target_db / kDecibelsPerDecade);
  if (settings.normalize == AudioNormalize::kPeak) {
    return target / peak;
  }
  return std::min(target / rms, 1 / peak);
}

AudioImport import_audio(std::span<const std::byte> encoded, const ImportSettings& settings) {
  const std::vector<std::byte> bytes{encoded.begin(), encoded.end()};
  AudioImport result;
  if (settings.normalize != AudioNormalize::kNone) {
    AudioDecoder measuring{bytes};
    const Levels levels = measure(measuring);
    result.gain = normalization_gain(levels.peak, levels.rms, settings);
  }

  const std::unique_ptr<OggOpusComments, CommentsDeleter> comments{ope_comments_create()};
  int error = OPE_OK;
  const OpusEncCallbacks callbacks{.write = write_bytes, .close = close_stream};
  const std::unique_ptr<OggOpusEnc, EncoderDeleter> encoder{ope_encoder_create_callbacks(
      &callbacks, &result.opus, comments.get(), static_cast<opus_int32>(kAudioSampleRate),
      static_cast<int>(kAudioChannels), kEncoderFamily, &error)};
  if (!encoder) {
    fail("cannot start the encoder", error);
  }

  AudioDecoder decoder{bytes};
  std::array<float, kBlockFrames * kAudioChannels> block{};
  const auto gain = static_cast<float>(result.gain);
  for (std::size_t frames = decoder.read(block); frames > 0; frames = decoder.read(block)) {
    for (float& sample : std::span{block}.first(frames * kAudioChannels)) {
      sample = std::clamp(sample * gain, -1.0F, 1.0F);
    }
    error = ope_encoder_write_float(encoder.get(), block.data(), static_cast<int>(frames));
    if (error != OPE_OK) {
      fail("cannot write the samples", error);
    }
  }
  error = ope_encoder_drain(encoder.get());
  if (error != OPE_OK) {
    fail("cannot finish the file", error);
  }
  return result;
}

}  // namespace vnmaki
