// Scaffold fuzz target: accepts any input and does nothing with it. It proves
// the libFuzzer toolchain works until real loaders get their own targets.

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  static_cast<void>(data);
  static_cast<void>(size);
  return 0;
}
