# HLSL shaders are compiled at build time with SDL_shadercross (docs/SPEC.md section 4) and
# embedded in the program, so no shader file has to ship next to it.
#
# vnmaki_add_shaders(<target> NAMESPACE <c++ namespace> SHADERS <name>.<vert|frag>.hlsl...
#                    [INCLUDE_DIRS <dir>...])
#
# For each shader, shadercross writes SPIR-V (Vulkan), MSL (Metal) and DXIL (Direct3D 12) plus
# its JSON resource reflection, and a generated C++ file defining
#
#   vnmaki::ShaderBytecode <namespace>::<name>_<vert|frag>();
#
# (engine/render/shader.hpp) is added to <target>. Declare the functions in a header of your own.
# Every .hlsli file in the include directories is a dependency of every shader.
#
# The shadercross executable is found on the PATH, in CMAKE_PREFIX_PATH, or among vcpkg's host
# tools (the sdl3-shadercross port). Set VNMAKI_SHADERCROSS to point at one explicitly.
# VNMAKI_SHADER_FORMATS selects the formats to build; a format left out is embedded empty, and
# the renderer can then not run on GPUs that need it.

find_program(VNMAKI_SHADERCROSS NAMES shadercross
             DOC "SDL_shadercross command-line compiler (HLSL to SPIR-V, MSL and DXIL)")
if(NOT VNMAKI_SHADERCROSS)
  message(
    FATAL_ERROR
      "shadercross (SDL_shadercross) was not found. With vcpkg it comes from the "
      "sdl3-shadercross host dependency in vcpkg.json; otherwise build it from "
      "https://github.com/libsdl-org/SDL_shadercross and add its install prefix to "
      "CMAKE_PREFIX_PATH, or set VNMAKI_SHADERCROSS to the executable.")
endif()

# Every format by default. On Apple platforms only Metal (MSL) runs, and DXC built for macOS has
# no DXIL signing library, so DXIL (Direct3D 12 only) is left out there.
if(APPLE)
  set(vnmaki_default_shader_formats "SPIRV;MSL")
else()
  set(vnmaki_default_shader_formats "SPIRV;MSL;DXIL")
endif()
set(VNMAKI_SHADER_FORMATS
    "${vnmaki_default_shader_formats}"
    CACHE STRING "Shader formats to compile HLSL to: a list of SPIRV, MSL, DXIL")

if(WIN32)
  find_program(VNMAKI_DXC NAMES dxc DOC "DirectX Shader Compiler, whose directory has dxil.dll")
endif()

function(vnmaki_add_shaders target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "NAMESPACE" "SHADERS;INCLUDE_DIRS")
  # DXC signs DXIL with dxil.dll, which it loads at run time from the PATH. vcpkg copies only
  # shadercross's link-time DLLs next to it, but keeps dxil.dll next to its dxc.exe.
  set(vnmaki_shadercross_launcher "")
  if(WIN32 AND VNMAKI_DXC)
    cmake_path(GET VNMAKI_DXC PARENT_PATH dxc_dir)
    set(vnmaki_shadercross_launcher "${CMAKE_COMMAND}" -E env --modify
                                    "PATH=path_list_prepend:${dxc_dir}" --)
  endif()
  set(embed_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/VnmakiEmbedShader.cmake")
  if(NOT arg_NAMESPACE OR NOT arg_SHADERS)
    message(FATAL_ERROR "vnmaki_add_shaders needs NAMESPACE and SHADERS")
  endif()

  set(include_args "")
  set(include_deps "")
  foreach(dir IN LISTS arg_INCLUDE_DIRS)
    cmake_path(ABSOLUTE_PATH dir BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
    list(APPEND include_args -I "${dir}")
    file(GLOB dir_headers CONFIGURE_DEPENDS "${dir}/*.hlsli")
    list(APPEND include_deps ${dir_headers})
  endforeach()

  set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
  set(all_generated "")
  foreach(shader IN LISTS arg_SHADERS)
    cmake_path(ABSOLUTE_PATH shader BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
    cmake_path(GET shader FILENAME file_name)
    if(NOT file_name MATCHES "^([A-Za-z0-9_]+)\\.(vert|frag)\\.hlsl$")
      message(FATAL_ERROR "shader file names look like <name>.vert.hlsl or <name>.frag.hlsl, "
                          "not '${file_name}'")
    endif()
    set(symbol "${CMAKE_MATCH_1}_${CMAKE_MATCH_2}")
    if(CMAKE_MATCH_2 STREQUAL "vert")
      set(stage vertex)
    else()
      set(stage fragment)
    endif()

    set(compiled "")
    set(commands "")
    foreach(format IN ITEMS SPIRV MSL DXIL JSON)
      if(format STREQUAL "JSON" OR format IN_LIST VNMAKI_SHADER_FORMATS)
        string(TOLOWER "${format}" extension)
        set(output "${out_dir}/${symbol}.${extension}")
        list(APPEND compiled "${output}")
        list(APPEND commands COMMAND ${vnmaki_shadercross_launcher} "${VNMAKI_SHADERCROSS}"
             "${shader}" -s HLSL -d ${format}
             -t ${stage} -e main ${include_args} -o "${output}")
        set(embed_${format} "${output}")
      else()
        set(embed_${format} "")
      endif()
    endforeach()

    cmake_path(RELATIVE_PATH shader BASE_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE
               shader_relative)
    set(generated "${out_dir}/${symbol}.cpp")
    add_custom_command(
      OUTPUT ${compiled} ${generated}
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${out_dir}" ${commands}
      COMMAND
        "${CMAKE_COMMAND}" "-DSOURCE=${shader_relative}" "-DNAMESPACE=${arg_NAMESPACE}"
        "-DSYMBOL=${symbol}" "-DSPIRV=${embed_SPIRV}" "-DMSL=${embed_MSL}" "-DDXIL=${embed_DXIL}"
        "-DJSON=${embed_JSON}" "-DOUTPUT=${generated}" -P "${embed_script}"
      DEPENDS "${shader}" ${include_deps} "${embed_script}"
      COMMENT "Compiling shader ${shader_relative} with shadercross"
      VERBATIM)
    target_sources(${target} PRIVATE "${generated}")
    # Generated data, not code to lint.
    set_source_files_properties("${generated}" TARGET_DIRECTORY ${target} PROPERTIES SKIP_LINTING
                                                                                  ON)
    list(APPEND all_generated "${generated}")
  endforeach()

  # A custom command only gets a build rule from a target in its own directory, and <target> may
  # live elsewhere (the engine library adds sources from module directories). This target owns the
  # rules; <target> waits for it.
  get_property(count GLOBAL PROPERTY vnmaki_shader_target_count)
  if(NOT count)
    set(count 0)
  endif()
  math(EXPR count "${count} + 1")
  set_property(GLOBAL PROPERTY vnmaki_shader_target_count ${count})
  set(shader_target "${target}_shaders_${count}")
  add_custom_target(${shader_target} DEPENDS ${all_generated})
  add_dependencies(${target} ${shader_target})
endfunction()
