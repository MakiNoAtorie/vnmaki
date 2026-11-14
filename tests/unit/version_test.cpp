#include "engine/core/version.hpp"

#include <string_view>

#include <doctest/doctest.h>

TEST_CASE("engine reports the version declared by the build") {
  const vnmaki::Version version = vnmaki::engine_version();

  CHECK(version.major == VNMAKI_EXPECTED_VERSION_MAJOR);
  CHECK(version.minor == VNMAKI_EXPECTED_VERSION_MINOR);
  CHECK(version.patch == VNMAKI_EXPECTED_VERSION_PATCH);
}

TEST_CASE("engine version string is major.minor.patch") {
  CHECK(vnmaki::engine_version_string() == std::string_view{VNMAKI_EXPECTED_VERSION});
}
