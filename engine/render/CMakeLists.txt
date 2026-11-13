# Render module: images, the renderer interface and its SDL GPU backend (docs/SPEC.md sections 6
# and 9).
target_sources(
  vnmaki_engine
  PRIVATE builtin_shaders.hpp
          frame_recorder.cpp
          frame_recorder.hpp
          image.cpp
          image.hpp
          renderer.cpp
          renderer.hpp
          shader.hpp)

# SDL GPU backend. Only files named sdl_* may include SDL (lint.no_sdl_outside_platform).
find_package(SDL3 CONFIG REQUIRED COMPONENTS SDL3)
target_sources(vnmaki_engine PRIVATE sdl_gpu_renderer.cpp sdl_gpu_renderer.hpp)
target_link_libraries(vnmaki_engine PRIVATE SDL3::SDL3)

# The engine's HLSL shaders, compiled to SPIR-V, MSL and DXIL at build time and embedded.
include(VnmakiShaders)
vnmaki_add_shaders(
  vnmaki_engine
  NAMESPACE vnmaki::shaders
  SHADERS "${PROJECT_SOURCE_DIR}/shaders/sprite.vert.hlsl"
          "${PROJECT_SOURCE_DIR}/shaders/sprite.frag.hlsl"
          "${PROJECT_SOURCE_DIR}/shaders/sdf_text.frag.hlsl"
  INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/shaders")

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
