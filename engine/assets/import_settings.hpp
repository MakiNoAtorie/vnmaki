#pragma once

// The import settings of project.toml (docs/SPEC.md section 19). They are read apart from
// engine/runtime/project_config.hpp, which ignores keys it does not know.
//
//   [build.audio]
//   normalize = "peak"   # "none" (default), "peak" or "rms": the gain that an audio file gets
//   target_db = -1.0     # the level to reach in dBFS; default -1 for "peak", -18 for "rms"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace vnmaki {

enum class AudioNormalize : std::uint8_t {
  kNone,  ///< The samples are not changed.
  kPeak,  ///< The loudest sample reaches `target_db`.
  kRms,   ///< The RMS level reaches `target_db`, but never so far that a sample clips.
};

struct ImportSettings {
  AudioNormalize normalize = AudioNormalize::kNone;
  double target_db = 0;  ///< Only used when `normalize` is not kNone; at most 0.

  friend bool operator==(const ImportSettings&, const ImportSettings&) = default;
};

inline constexpr double kDefaultPeakTargetDb = -1;
inline constexpr double kDefaultRmsTargetDb = -18;

/// A key of `[build.audio]` has a wrong value. The message starts with "project.toml".
class ImportSettingsError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Reads the settings from the text of a project.toml (which `parse_project_config` has accepted,
/// so the TOML is valid). Throws ImportSettingsError.
[[nodiscard]] ImportSettings parse_import_settings(std::string_view toml_text);

}  // namespace vnmaki
