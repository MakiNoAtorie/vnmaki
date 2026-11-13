// Fuzzes the Lua tokenizer and the story scan (engine/tools/script_scan.hpp). Scripts are
// untrusted input for `vnmaki graph` and the dev overlay. The properties are in
// script_scan_properties.hpp.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "fuzz/script_scan_properties.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::uint8_t> bytes{data, size};
  std::string source;
  source.reserve(size);
  for (const std::uint8_t byte : bytes) {
    source.push_back(static_cast<char>(byte));
  }
  vnmaki::fuzz::check_script_scan_properties(source);
  return 0;
}
