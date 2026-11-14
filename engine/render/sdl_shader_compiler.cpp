#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "engine/render/shader_compiler.hpp"
#include "engine/render/shader_headers.hpp"

#if VNMAKI_RUNTIME_SHADERS
#include <SDL3/SDL.h>
#include <SDL3_shadercross/SDL_shadercross.h>
#endif

namespace vnmaki {

#if VNMAKI_RUNTIME_SHADERS

namespace {

constexpr std::string_view kIncludeKeyword = "#include";
constexpr int kMaxIncludeDepth = 8;

// The name in a line `#include "name"`, or empty.
std::string_view included_name(std::string_view line) {
  const std::size_t start = line.find_first_not_of(" \t");
  if (start == std::string_view::npos || !line.substr(start).starts_with(kIncludeKeyword)) {
    return {};
  }
  const std::size_t open = line.find('"', start + kIncludeKeyword.size());
  const std::size_t close = open == std::string_view::npos ? open : line.find('"', open + 1);
  return close == std::string_view::npos ? std::string_view{}
                                         : line.substr(open + 1, close - open - 1);
}

// Appends `source` with each include of an engine header replaced by the header's text. `#line`
// directives keep the file names and line numbers of compiler messages right.
void expand_includes(std::string_view name, std::string_view source, int depth,
                     std::string& output) {
  std::size_t line_number = 1;
  while (!source.empty()) {
    const std::size_t end = source.find('\n');
    const std::string_view line = source.substr(0, end);
    source = end == std::string_view::npos ? std::string_view{} : source.substr(end + 1);
    const std::string_view header_name = included_name(line);
    const std::string_view header =
        header_name.empty() ? std::string_view{} : hlsl_header(header_name);
    if (!header.empty() && depth < kMaxIncludeDepth) {
      output += "#line 1 \"" + std::string{header_name} + "\"\n";
      expand_includes(header_name, header, depth + 1, output);
      output += "\n#line " + std::to_string(line_number + 1) + " \"" + std::string{name} + "\"\n";
    } else {
      output += line;
      output += '\n';
    }
    ++line_number;
  }
}

struct SdlFree {
  void operator()(void* memory) const { SDL_free(memory); }
};
using SdlBuffer = std::unique_ptr<void, SdlFree>;

[[noreturn]] void fail(std::string_view what) {
  std::string message{what};
  const std::string_view reason = SDL_GetError();
  if (!reason.empty()) {
    message += ": ";
    message += reason;
  }
  throw ShaderCompileError{message};
}

// SDL_shadercross loads DXC here. It must be initialized once, from one thread.
bool initialize_compiler() {
  static const bool ok = SDL_ShaderCross_Init();
  return ok;
}

std::vector<std::uint8_t> bytes_of(const SdlBuffer& buffer, std::size_t size) {
  const auto* begin = static_cast<const std::uint8_t*>(buffer.get());
  return {begin, begin + size};
}

}  // namespace

bool shader_compiler_available() { return initialize_compiler(); }

CompiledShader compile_fragment_shader(std::string_view name, std::string_view hlsl) {
  if (!initialize_compiler()) {
    fail("cannot start the shader compiler");
  }
  std::string source;
  source += "#line 1 \"" + std::string{name} + "\"\n";
  expand_includes(name, hlsl, 0, source);

  SDL_ShaderCross_HLSL_Info hlsl_info{};
  hlsl_info.source = source.c_str();
  hlsl_info.entrypoint = "main";
  hlsl_info.shader_stage = SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;
  std::size_t spirv_size = 0;
  const SdlBuffer spirv{SDL_ShaderCross_CompileSPIRVFromHLSL(&hlsl_info, &spirv_size)};
  if (!spirv) {
    fail("cannot compile the shader");
  }

  CompiledShader result;
  result.spirv = bytes_of(spirv, spirv_size);

  const std::unique_ptr<SDL_ShaderCross_GraphicsShaderMetadata, SdlFree> metadata{
      SDL_ShaderCross_ReflectGraphicsSPIRV(result.spirv.data(), result.spirv.size(), 0)};
  if (!metadata) {
    fail("cannot read the shader's resources");
  }
  if (metadata->resource_info.num_storage_textures != 0 ||
      metadata->resource_info.num_storage_buffers != 0) {
    throw ShaderCompileError{std::string{name} +
                             ": storage textures and buffers are not supported in shaders"};
  }
  result.samplers = metadata->resource_info.num_samplers;
  result.uniform_buffers = metadata->resource_info.num_uniform_buffers;

  SDL_ShaderCross_SPIRV_Info spirv_info{};
  spirv_info.bytecode = result.spirv.data();
  spirv_info.bytecode_size = result.spirv.size();
  spirv_info.entrypoint = "main";
  spirv_info.shader_stage = SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;

  const SdlBuffer msl{SDL_ShaderCross_TranspileMSLFromSPIRV(&spirv_info)};
  if (!msl) {
    fail("cannot translate the shader to Metal");
  }
  result.msl = bytes_of(msl, SDL_strlen(static_cast<const char*>(msl.get())));

#ifndef __APPLE__
  std::size_t dxil_size = 0;
  const SdlBuffer dxil{SDL_ShaderCross_CompileDXILFromSPIRV(&spirv_info, &dxil_size)};
  if (!dxil) {
    fail("cannot translate the shader to Direct3D 12");
  }
  result.dxil = bytes_of(dxil, dxil_size);
#endif
  return result;
}

#else

bool shader_compiler_available() { return false; }

CompiledShader compile_fragment_shader(std::string_view /*name*/, std::string_view /*hlsl*/) {
  throw ShaderCompileError{"this build has no run-time shader compiler (SDL_shadercross)"};
}

#endif

}  // namespace vnmaki
