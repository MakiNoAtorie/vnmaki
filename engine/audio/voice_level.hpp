#pragma once

// The loudness of the voice that plays (docs/SPEC.md section 10, lip sync). Audio
// (engine/audio/audio.hpp) implements this, so the scene needs no mixer.

#include <optional>
#include <string_view>

namespace vnmaki {

class VoiceLevel {
 public:
  VoiceLevel() = default;
  VoiceLevel(const VoiceLevel&) = default;
  VoiceLevel(VoiceLevel&&) = default;
  VoiceLevel& operator=(const VoiceLevel&) = default;
  VoiceLevel& operator=(VoiceLevel&&) = default;
  virtual ~VoiceLevel() = default;

  /// The RMS level, from 0 to 1, of the voice with `tag` that plays now: the file's samples,
  /// before the volume, over the last 20 ms that were mixed. Empty when no voice with that tag
  /// plays.
  [[nodiscard]] virtual std::optional<double> voice_level(std::string_view tag) const = 0;
};

}  // namespace vnmaki
