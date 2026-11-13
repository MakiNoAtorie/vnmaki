# Render module: images, the renderer interface and its SDL GPU backend (docs/SPEC.md sections 6
# and 9).
target_sources(
  vnmaki_engine
  PRIVATE builtin_shaders.hpp
          frame_recorder.cpp
          frame_recorder.hpp
          image.cpp
          image.hpp
          mesh.hpp
          renderer.cpp
          renderer.hpp
          precompiled_shader.cpp
          precompiled_shader.hpp
          shader.hpp
          shader_compiler.hpp
          shader_headers.hpp)

# SDL GPU backend. Only files named sdl_* may include SDL (lint.no_sdl_outside_platform).
find_package(SDL3 CONFIG REQUIRED COMPONENTS SDL3)
target_sources(vnmaki_engine PRIVATE sdl_gpu_renderer.cpp sdl_gpu_renderer.hpp)
target_link_libraries(vnmaki_engine PRIVATE SDL3::SDL3)

# Run-time compiler for a game's shaders (sdl_shader_compiler.cpp). macOS has no vcpkg port of the
# library (vcpkg.json), and a release build (VNMAKI_SHADER_COMPILER off) does not link it: there the
# compiler is a stub that throws, and `shaders/<name>.frag.vnshader` files from `vnmaki import` load.
set(vnmaki_runtime_shaders 0)
if(VNMAKI_SHADER_COMPILER)
  find_package(SDL3_shadercross CONFIG)
endif()
if(TARGET SDL3_shadercross::SDL3_shadercross)
  set(vnmaki_runtime_shaders 1)
  target_link_libraries(vnmaki_engine PRIVATE SDL3_shadercross::SDL3_shadercross)
endif()
target_sources(vnmaki_engine PRIVATE sdl_shader_compiler.cpp)
target_compile_definitions(vnmaki_engine PRIVATE VNMAKI_RUNTIME_SHADERS=${vnmaki_runtime_shaders})

# The headers a game shader includes, embedded as text for the run-time compiler.
foreach(header IN ITEMS vnmaki sprite_fragment transition mesh)
  set(header_path "${PROJECT_SOURCE_DIR}/shaders/${header}.hlsli")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${header_path}")
  string(TOUPPER "${header}" header_upper)
  file(READ "${header_path}" VNMAKI_${header_upper}_HLSLI)
endforeach()
configure_file(shader_headers.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/shader_headers.cpp" @ONLY)
target_sources(vnmaki_engine PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shader_headers.cpp")
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/shader_headers.cpp" TARGET_DIRECTORY
                            vnmaki_engine PROPERTIES SKIP_LINTING ON)

# The engine's HLSL shaders, compiled to SPIR-V, MSL and DXIL at build time and embedded.
include(VnmakiShaders)
vnmaki_add_shaders(
  vnmaki_engine
  NAMESPACE vnmaki::shaders
  SHADERS "${PROJECT_SOURCE_DIR}/shaders/sprite.vert.hlsl"
          "${PROJECT_SOURCE_DIR}/shaders/sprite.frag.hlsl"
          "${PROJECT_SOURCE_DIR}/shaders/sdf_text.frag.hlsl"
          "${PROJECT_SOURCE_DIR}/shaders/transition_dissolve.frag.hlsl"
          "${PROJECT_SOURCE_DIR}/shaders/transition_fade.frag.hlsl"
          "${PROJECT_SOURCE_DIR}/shaders/transition_slide.frag.hlsl"
          "${PROJECT_SOURCE_DIR}/shaders/transition_wipe.frag.hlsl"
          "${PROJECT_SOURCE_DIR}/shaders/transition_mask.frag.hlsl"
          "${PROJECT_SOURCE_DIR}/shaders/mesh.vert.hlsl"
          "${PROJECT_SOURCE_DIR}/shaders/mesh_unlit.frag.hlsl"
          "${PROJECT_SOURCE_DIR}/shaders/mesh_toon.frag.hlsl"
          "${PROJECT_SOURCE_DIR}/shaders/mesh_pbr.frag.hlsl"
  INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/shaders")

# libwebp decodes WebP. vcpkg and Debian/Ubuntu (libwebp-dev) both ship a CMake config.
find_package(WebP CONFIG REQUIRED)
target_link_libraries(vnmaki_engine PRIVATE WebP::webp)

# stb_image decodes PNG and JPEG. vcpkg's stb installs stb_image.h at the top of its include
# directory, Debian/Ubuntu's libstb-dev under stb/.
find_path(VNMAKI_STB_INCLUDE_DIR stb_image.h PATH_SUFFIXES stb REQUIRED)
target_include_directories(vnmaki_engine SYSTEM PRIVATE "${VNMAKI_STB_INCLUDE_DIR}")
# The implementation is third-party code: compile it without the project's warnings, and keep
# clang-tidy off it. It still gets the sanitizer and fuzzing instrumentation from vnmaki::options,
# so the image fuzz target covers stb_image itself.
target_sources(vnmaki_engine PRIVATE stb_image_impl.cpp)
if(MSVC)
  set(vnmaki_third_party_flags /W0)
else()
  set(vnmaki_third_party_flags -w)
  # stb_image calls memcpy(NULL, p, 0) for a zero-length PNG IDAT chunk, which is valid PNG
  # (found by fuzz_image). That is harmless unless the optimizer uses memcpy's nonnull contract
  # to delete stb's later null checks, so forbid that, and keep UBSan from failing on that one
  # known pattern. tests/unit/image_test.cpp decodes such a PNG.
  list(APPEND vnmaki_third_party_flags -fno-delete-null-pointer-checks)
  if("undefined" IN_LIST VNMAKI_SANITIZERS)
    list(APPEND vnmaki_third_party_flags -fno-sanitize=nonnull-attribute)
  endif()
endif()
set_source_files_properties(
  stb_image_impl.cpp TARGET_DIRECTORY vnmaki_engine PROPERTIES COMPILE_OPTIONS
                                                               "${vnmaki_third_party_flags}"
                                                               SKIP_LINTING ON)
