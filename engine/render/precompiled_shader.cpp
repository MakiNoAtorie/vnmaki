#include "engine/render/precompiled_shader.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>

#include "engine/core/byte_stream.hpp"

namespace vnmaki {

namespace {

constexpr std::array<char, 4> kMagic{'V', 'N', 'S', 'H'};
constexpr std::uint32_t kVersion = 1;
// A shader with three textures and a buffer is a few kilobytes; the cap only stops a bad length.
constexpr std::size_t kMaxBlock = std::size_t{16} << 20;

std::vector<std::uint8_t> bytes_from(std::span<const std::byte> block) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(block.size());
  for (const std::byte byte : block) {
    bytes.push_back(std::to_integer<std::uint8_t>(byte));
  }
  return bytes;
}

std::vector<std::byte> block_from(const std::vector<std::uint8_t>& bytes) {
  std::vector<std::byte> block;
  block.reserve(bytes.size());
  for (const std::uint8_t byte : bytes) {
    block.push_back(static_cast<std::byte>(byte));
  }
  return block;
}

}  // namespace

std::vector<std::byte> write_precompiled_shader(const CompiledShader& shader) {
  ByteWriter writer;
  for (const char c : kMagic) {
    writer.u8(static_cast<std::uint8_t>(c));
  }
  writer.u32(kVersion);
  writer.u32(shader.samplers);
  writer.u32(shader.uniform_buffers);
  writer.block(block_from(shader.spirv));
  writer.block(block_from(shader.msl));
  writer.block(block_from(shader.dxil));
  return writer.take();
}

CompiledShader read_precompiled_shader(std::span<const std::byte> bytes) {
  try {
    ByteReader reader{bytes, "precompiled shader"};
    for (const char c : kMagic) {
      if (reader.u8() != static_cast<std::uint8_t>(c)) {
        throw ShaderCompileError{"is not a precompiled shader"};
      }
    }
    if (reader.u32() != kVersion) {
      throw ShaderCompileError{"is a precompiled shader of another version"};
    }
    CompiledShader shader;
    shader.samplers = reader.u32();
    shader.uniform_buffers = reader.u32();
    for (std::vector<std::uint8_t>* target : {&shader.spirv, &shader.msl, &shader.dxil}) {
      const std::span<const std::byte> block = reader.block();
      if (block.size() > kMaxBlock) {
        throw ShaderCompileError{"is a damaged precompiled shader"};
      }
      *target = bytes_from(block);
    }
    if (!reader.at_end()) {
      throw ShaderCompileError{"is a damaged precompiled shader"};
    }
    return shader;
  } catch (const std::invalid_argument&) {
    throw ShaderCompileError{"is a damaged precompiled shader"};
  }
}

}  // namespace vnmaki
