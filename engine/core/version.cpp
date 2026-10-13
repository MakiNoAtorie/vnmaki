#include "engine/core/version.hpp"

namespace vnmaki {

Version engine_version() noexcept {
  return {
      .major = VNMAKI_VERSION_MAJOR, .minor = VNMAKI_VERSION_MINOR, .patch = VNMAKI_VERSION_PATCH};
}

std::string_view engine_version_string() noexcept { return VNMAKI_VERSION_STRING; }

}  // namespace vnmaki
