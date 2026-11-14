#pragma once

#include <string_view>

namespace vnmaki {

/// Semantic version of the engine.
struct Version {
  int major = 0;
  int minor = 0;
  int patch = 0;

  friend constexpr bool operator==(const Version&, const Version&) = default;
};

/// The version of the engine this binary was built from.
[[nodiscard]] Version engine_version() noexcept;

/// The engine version as "major.minor.patch".
[[nodiscard]] std::string_view engine_version_string() noexcept;

}  // namespace vnmaki
