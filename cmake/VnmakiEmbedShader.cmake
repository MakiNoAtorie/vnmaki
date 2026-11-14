# Writes a C++ file that embeds one compiled shader (see VnmakiShaders.cmake).
#
# Usage: cmake -DSOURCE=<hlsl path for the comment> -DNAMESPACE=<ns> -DSYMBOL=<function name>
#              -DSPIRV=<file or empty> -DMSL=<file or empty> -DDXIL=<file or empty>
#              -DJSON=<shadercross reflection> -DOUTPUT=<cpp file> -P VnmakiEmbedShader.cmake

function(byte_array_initializer file out_var out_size)
  if(file STREQUAL "")
    set(${out_var} "" PARENT_SCOPE)
    set(${out_size} 0 PARENT_SCOPE)
    return()
  endif()
  file(READ "${file}" hex HEX)
  string(LENGTH "${hex}" hex_length)
  math(EXPR size "${hex_length} / 2")
  if(size EQUAL 0)
    message(FATAL_ERROR "shadercross wrote an empty file: ${file}")
  endif()
  # 16 bytes per line.
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
  string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){16})" "\\1\n    " bytes "${bytes}")
  set(${out_var} "\n    ${bytes}\n" PARENT_SCOPE)
  set(${out_size} ${size} PARENT_SCOPE)
endfunction()

file(READ "${JSON}" reflection)
foreach(field IN ITEMS samplers uniform_buffers storage_textures storage_buffers)
  string(JSON ${field} GET "${reflection}" ${field})
endforeach()
if(NOT storage_textures EQUAL 0 OR NOT storage_buffers EQUAL 0)
  message(FATAL_ERROR "${SOURCE}: storage textures and buffers are not supported in VNMaki "
                      "shaders yet")
endif()

byte_array_initializer("${SPIRV}" spirv_bytes spirv_size)
byte_array_initializer("${MSL}" msl_bytes msl_size)
byte_array_initializer("${DXIL}" dxil_bytes dxil_size)

set(content
    "// Generated from ${SOURCE} by cmake/VnmakiEmbedShader.cmake. Do not edit.

#include <array>
#include <cstdint>

#include \"engine/render/shader.hpp\"

// NOLINTBEGIN
namespace ${NAMESPACE} {

namespace {

constexpr std::array<std::uint8_t, ${spirv_size}> kSpirv{${spirv_bytes}};
constexpr std::array<std::uint8_t, ${msl_size}> kMsl{${msl_bytes}};
constexpr std::array<std::uint8_t, ${dxil_size}> kDxil{${dxil_bytes}};

}  // namespace

vnmaki::ShaderBytecode ${SYMBOL}() {
  return vnmaki::ShaderBytecode{
      .spirv = kSpirv,
      .msl = kMsl,
      .dxil = kDxil,
      .samplers = ${samplers},
      .uniform_buffers = ${uniform_buffers},
  };
}

}  // namespace ${NAMESPACE}
// NOLINTEND
")

file(WRITE "${OUTPUT}" "${content}")
