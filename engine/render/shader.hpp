#pragma once

#include <cstdint>
#include <span>

namespace vnmaki {

/// One shader stage compiled from HLSL at build time (cmake/VnmakiShaders.cmake), in every
/// format a backend may need. The entry point is `main` in SPIR-V and DXIL and `main0` in MSL
/// (SPIRV-Cross renames it). A format the build left out is empty.
struct ShaderBytecode {
  std::span<const std::uint8_t> spirv;  ///< Vulkan.
  std::span<const std::uint8_t> msl;    ///< Metal (source text, not NUL-terminated).
  std::span<const std::uint8_t> dxil;   ///< Direct3D 12.
  /// Resource counts from shadercross's reflection.
  std::uint32_t samplers = 0;
  std::uint32_t uniform_buffers = 0;
};

}  // namespace vnmaki
