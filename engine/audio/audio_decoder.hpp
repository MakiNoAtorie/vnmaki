#pragma once

// Audio decoding (docs/SPEC.md section 14): Opus, Vorbis, FLAC and WAV, decoded a block at a time
// to the mixer's format. Audio files ship with games and mods, so they are untrusted input.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace vnmaki {

/// Every decoder gives this format: interleaved float frames, left then right.
inline constexpr std::uint32_t kAudioSampleRate = 48'000;
inline constexpr std::size_t kAudioChannels = 2;

/// The data is not audio the engine can decode. what() completes the sentence "<file> ...", e.g.
/// "is not an Opus, Vorbis, FLAC or WAV file".
class AudioError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Where a looping track jumps back: on reaching frame `end` it continues at frame `start`.
/// Frames are counted at kAudioSampleRate. `end` 0 means the end of the file.
struct AudioLoopPoints {
  std::uint64_t start = 0;
  std::uint64_t end = 0;

  friend bool operator==(const AudioLoopPoints&, const AudioLoopPoints&) = default;
};

/// One encoded file, decoded on demand. Mono files play on both channels, files with more
/// channels are mixed down, and other sample rates are resampled.
///
/// Not thread-safe: one thread at a time.
class AudioDecoder {
 public:
  /// Takes the bytes of an Opus or Vorbis file (in Ogg), a FLAC file or a WAV file. Throws
  /// AudioError for anything else, and for more than kMaxSourceChannels channels or a sample
  /// rate outside kMinSourceRate..kMaxSourceRate.
  explicit AudioDecoder(std::vector<std::byte> encoded);
  AudioDecoder(const AudioDecoder&) = delete;
  AudioDecoder(AudioDecoder&&) noexcept;
  AudioDecoder& operator=(const AudioDecoder&) = delete;
  AudioDecoder& operator=(AudioDecoder&&) noexcept;
  ~AudioDecoder();

  /// Decodes the next frames into `out` (kAudioChannels floats per frame, each from -1 to 1) and
  /// returns how many frames it wrote. Fewer than `out` holds means the file ended (or the rest
  /// of it is damaged).
  std::size_t read(std::span<float> out);

  /// Continues at `frame`. Returns false, and then read() gives nothing more, when the file
  /// cannot be read there.
  bool seek(std::uint64_t frame);

  /// The loop points the file's tags give, for Opus and Vorbis files: LOOPSTART with LOOPLENGTH
  /// or LOOPEND, in samples of the file.
  [[nodiscard]] std::optional<AudioLoopPoints> loop_points() const;

  static constexpr std::uint32_t kMaxSourceChannels = 8;
  static constexpr std::uint32_t kMinSourceRate = 8'000;
  static constexpr std::uint32_t kMaxSourceRate = 192'000;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vnmaki
