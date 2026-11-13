// Fuzzes the `.po` reader and writer (engine/l10n/po.hpp). Translation files are edited by other
// tools and by people, so they are untrusted input for `vnmaki extract-strings` and `vnmaki
// check`. The properties are in po_properties.hpp.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "fuzz/po_properties.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text{reinterpret_cast<const char*>(data), size};
  vnmaki::fuzz::check_po_read_properties(text);
  vnmaki::fuzz::check_po_write_properties(text);
  return 0;
}
