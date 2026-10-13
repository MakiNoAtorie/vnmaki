#pragma once

// Shaders only the renderer tests use (tests/render/shaders), compiled like the engine's own.

#include <array>

#include "engine/render/shader.hpp"

namespace vnmaki::test_shaders {

/// tests/render/shaders/grayscale.frag.hlsl; its constant buffer is GrayscaleUniforms.
[[nodiscard]] ShaderBytecode grayscale_frag();

/// The layout of grayscale.frag.hlsl's constant buffer (one 16-byte row).
struct GrayscaleUniforms {
  float amount = 0;
  std::array<float, 3> unused_padding{};
};

}  // namespace vnmaki::test_shaders
