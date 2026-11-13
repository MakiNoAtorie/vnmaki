// Fuzzes the WebP path of the image decoder (libwebp through engine/render/image.hpp). Random
// bytes almost never make a "RIFF....WEBP" header, so the input here is the data of the VP8L
// chunk: the target adds the container around it. The properties are in
// fuzz/image_properties.hpp.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "fuzz/image_properties.hpp"

namespace {

constexpr std::size_t kLengthBytes = 4;
constexpr std::size_t kChunkHeaderBytes = 8;  // a tag and a length
constexpr unsigned kByteBits = 8;
constexpr unsigned kByteMask = 0xFF;

void append_tag(std::vector<std::byte>& bytes, std::string_view tag) {
  for (const char letter : tag) {
    bytes.push_back(static_cast<std::byte>(letter));
  }
}

void append_length(std::vector<std::byte>& bytes, std::size_t length) {
  for (std::size_t index = 0; index < kLengthBytes; ++index) {
    bytes.push_back(static_cast<std::byte>((length >> (kByteBits * index)) & kByteMask));
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  std::vector<std::byte> file;
  file.reserve(size + (2 * kChunkHeaderBytes) + 1);
  append_tag(file, "RIFF");
  const std::size_t padded_size = size + (size % 2);  // chunks have an even size
  append_length(file, kLengthBytes + kChunkHeaderBytes + padded_size);
  append_tag(file, "WEBP");
  append_tag(file, "VP8L");
  append_length(file, size);
  const std::span<const std::byte> payload = std::as_bytes(std::span{data, size});
  file.insert(file.end(), payload.begin(), payload.end());
  file.resize(file.size() + (size % 2));
  vnmaki::fuzz::check_image_properties(file);
  return 0;
}
