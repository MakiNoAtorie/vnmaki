#pragma once

// A game shader compiled by `vnmaki import` (docs/SPEC.md section 19) so that a release build, which
// has no shader compiler, can load it. The file is `shaders/<name>.frag.vnshader`.

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "engine/render/shader_compiler.hpp"

namespace vnmaki {

inline constexpr std::string_view kPrecompiledShaderExtension = ".vnshader";

/// The file form: the magic "VNSH", a u32 version, the sampler and uniform buffer counts (u32), then
/// the SPIR-V, MSL and DXIL blocks (a u32 length and the bytes; engine/core/byte_stream.hpp).
[[nodiscard]] std::vector<std::byte> write_precompiled_shader(const CompiledShader& shader);

/// Reads that form. Throws ShaderCompileError for any other bytes. The message completes the
/// sentence "<file> ...".
[[nodiscard]] CompiledShader read_precompiled_shader(std::span<const std::byte> bytes);

}  // namespace vnmaki
