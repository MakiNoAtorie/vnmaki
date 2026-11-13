#pragma once

// The little-endian byte form of snapshot state: what SceneState::capture() writes and
// restore() reads. Integers are little-endian, floats are their IEEE 754 bits, and a string or
// byte block is a u32 length followed by the bytes.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vnmaki {

class ByteWriter {
 public:
  void u8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }
  void u32(std::uint32_t value) { integer(value, sizeof(value)); }
  void u64(std::uint64_t value) { integer(value, sizeof(value)); }
  void f32(float value) { u32(std::bit_cast<std::uint32_t>(value)); }
  void f64(double value) { u64(std::bit_cast<std::uint64_t>(value)); }
  void string(std::string_view text) {
    u32(static_cast<std::uint32_t>(text.size()));
    for (const char c : text) {
      bytes_.push_back(static_cast<std::byte>(c));
    }
  }
  void block(std::span<const std::byte> bytes) {
    u32(static_cast<std::uint32_t>(bytes.size()));
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
  }
  [[nodiscard]] std::vector<std::byte> take() { return std::move(bytes_); }

 private:
  void integer(std::uint64_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
      bytes_.push_back(static_cast<std::byte>((value >> (i * kBitsPerByte)) & kByteMask));
    }
  }

  static constexpr unsigned kBitsPerByte = 8;
  static constexpr std::uint64_t kByteMask = 0xFF;

  std::vector<std::byte> bytes_;
};

/// Throws std::invalid_argument ("<what>: the data ends early") when the bytes end early.
class ByteReader {
 public:
  /// `what` names the state in error messages, e.g. "scene state". It must outlive the reader.
  ByteReader(std::span<const std::byte> bytes, std::string_view what)
      : bytes_(bytes), what_(what) {}

  [[nodiscard]] bool at_end() const { return bytes_.empty(); }
  std::uint8_t u8() { return std::to_integer<std::uint8_t>(take(1).front()); }
  std::uint32_t u32() { return static_cast<std::uint32_t>(integer(sizeof(std::uint32_t))); }
  std::uint64_t u64() { return integer(sizeof(std::uint64_t)); }
  float f32() { return std::bit_cast<float>(u32()); }
  double f64() { return std::bit_cast<double>(u64()); }
  std::string string() {
    const std::span<const std::byte> bytes = block();
    std::string text;
    text.reserve(bytes.size());
    for (const std::byte byte : bytes) {
      text.push_back(std::to_integer<char>(byte));
    }
    return text;
  }
  /// A view into the bytes the reader was given.
  std::span<const std::byte> block() { return take(u32()); }

 private:
  std::uint64_t integer(std::size_t size) {
    std::uint64_t value = 0;
    unsigned shift = 0;
    for (const std::byte byte : take(size)) {
      value |= std::to_integer<std::uint64_t>(byte) << shift;
      shift += kBitsPerByte;
    }
    return value;
  }

  std::span<const std::byte> take(std::size_t count) {
    if (count > bytes_.size()) {
      throw std::invalid_argument{std::string{what_} + ": the data ends early"};
    }
    const std::span<const std::byte> taken = bytes_.first(count);
    bytes_ = bytes_.subspan(count);
    return taken;
  }

  static constexpr unsigned kBitsPerByte = 8;

  std::span<const std::byte> bytes_;
  std::string_view what_;
};

}  // namespace vnmaki
