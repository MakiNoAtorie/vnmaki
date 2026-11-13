# Asset import (docs/SPEC.md section 19): the import cache, and the importers for audio, fonts,
# glTF and game shaders. They build in every preset.
target_sources(
  vnmaki_engine
  PRIVATE audio_import.cpp
          audio_import.hpp
          font_import.cpp
          font_import.hpp
          gltf_import.cpp
          gltf_import.hpp
          import_cache.cpp
          import_cache.hpp
          import_settings.cpp
          import_settings.hpp
          importer.cpp
          importer.hpp)

# libopusenc has no CMake config. vcpkg and Debian (libopusenc-dev) install opusenc.h under opus/.
find_path(VNMAKI_OPUSENC_INCLUDE_DIR opusenc.h PATH_SUFFIXES opus REQUIRED)
find_library(VNMAKI_OPUSENC_LIBRARY opusenc REQUIRED)
target_include_directories(vnmaki_engine SYSTEM PRIVATE "${VNMAKI_OPUSENC_INCLUDE_DIR}")
target_link_libraries(vnmaki_engine PRIVATE "${VNMAKI_OPUSENC_LIBRARY}")

# HarfBuzz subsetting: vcpkg's config has the target harfbuzz::harfbuzz-subset, Debian ships the
# plain library (libharfbuzz-subset0, in libharfbuzz-dev).
find_package(harfbuzz CONFIG REQUIRED)
if(TARGET harfbuzz::harfbuzz-subset)
  target_link_libraries(vnmaki_engine PRIVATE harfbuzz::harfbuzz-subset)
else()
  find_library(VNMAKI_HARFBUZZ_SUBSET_LIBRARY harfbuzz-subset REQUIRED)
  target_link_libraries(vnmaki_engine PRIVATE "${VNMAKI_HARFBUZZ_SUBSET_LIBRARY}")
endif()

# cgltf is one header. vcpkg installs cgltf.h at the top of its include directory.
find_path(VNMAKI_CGLTF_INCLUDE_DIR cgltf.h PATH_SUFFIXES cgltf REQUIRED)
target_include_directories(vnmaki_engine SYSTEM PRIVATE "${VNMAKI_CGLTF_INCLUDE_DIR}")
# The implementation of cgltf is engine/model3d/cgltf_impl.cpp (the only file that defines
# CGLTF_IMPLEMENTATION, compiled without the project's warnings).
