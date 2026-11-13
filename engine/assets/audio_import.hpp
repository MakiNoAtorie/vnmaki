#pragma once

// The audio importer (docs/SPEC.md section 19): a WAV or FLAC file becomes an Ogg Opus file
// (libopusenc), with a gain that brings its peak or RMS level to the target of project.toml.
// The result is stereo at 48 kHz, as every decoder of the engine gives it.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/assets/import_settings.hpp"

namespace vnmaki {

/// Importer name and version of the cache key. Raise the version when the output changes.
inline constexpr char kAudioImporter[] = "audio";
inline constexpr std::uint32_t kAudioImporterVersion = 1;

/// The gain (a factor) that settings.normalize asks for a file with this `peak` (the largest
/// absolute sample, 0 to 1) and `rms` (over all samples). 1 for kNone and for silence. For kRms it
/// is at most `1 / peak`, so that no sample clips.
[[nodiscard]] double normalization_gain(double peak, double rms, const ImportSettings& settings);

struct AudioImport {
  std::vector<std::byte> opus{};
  double gain = 1;  ///< The factor the samples were multiplied by.
};

/// Decodes a WAV, FLAC, Vorbis or Opus file and encodes it as Opus. Throws AudioError if the
/// data cannot be decoded and std::runtime_error if the encoder fails.
[[nodiscard]] AudioImport import_audio(std::span<const std::byte> encoded,
                                       const ImportSettings& settings);

}  // namespace vnmaki
