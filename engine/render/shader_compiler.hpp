#pragma once

// Compiles a game's HLSL fragment shader when the game loads (docs/SPEC.md sections 4 and 9), with
// the SDL_shadercross library. The engine's own shaders are compiled at build time instead
// (cmake/VnmakiShaders.cmake).

#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "engine/render/shader.hpp"

namespace vnmaki {

/// Thrown for HLSL that does not compile or a compiler that cannot run. The message of a
/// compile error is the compiler's: "<name>:<line>:<column>: error: <message>".
class ShaderCompileError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// A compiled fragment shader that owns its bytes. A format that this platform cannot make is
/// empty (DXIL on macOS).
struct CompiledShader {
  std::vector<std::uint8_t> spirv;
  std::vector<std::uint8_t> msl;
  std::vector<std::uint8_t> dxil;
  std::uint32_t samplers = 0;
  std::uint32_t uniform_buffers = 0;

  /// The view IRenderer::create_shader takes; valid while this object lives.
  [[nodiscard]] ShaderBytecode bytecode() const {
    return ShaderBytecode{.spirv = spirv,
                          .msl = msl,
                          .dxil = dxil,
                          .samplers = samplers,
                          .uniform_buffers = uniform_buffers};
  }
};

/// True when this build can compile shaders at run time and the compiler loads. It is false
/// on a platform whose build has no SDL_shadercross library (macOS) or without DXC.
[[nodiscard]] bool shader_compiler_available();

/// Compiles the fragment shader `hlsl` (entry point `main`, see shaders/vnmaki.hlsli) to every
/// format. `#include "vnmaki.hlsli"` finds the engine's header. `name` is the file name that
/// errors and `#line` directives show. Throws ShaderCompileError, also when the shader uses
/// storage buffers or textures.
[[nodiscard]] CompiledShader compile_fragment_shader(std::string_view name, std::string_view hlsl);

}  // namespace vnmaki
