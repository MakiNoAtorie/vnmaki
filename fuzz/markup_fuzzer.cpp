// Fuzzes the text tag parser (engine/text/markup.hpp). Dialogue text comes from scripts,
// translations and mods, so it is untrusted input. The properties are in markup_properties.hpp.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "fuzz/markup_properties.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::uint8_t> bytes{data, size};
  std::string markup;
  markup.reserve(size);
  for (const std::uint8_t byte : bytes) {
    markup.push_back(static_cast<char>(byte));
  }
  vnmaki::fuzz::check_markup_properties(markup);
  return 0;
}
