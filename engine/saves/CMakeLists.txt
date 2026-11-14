# Saves module (ticket 12): the save file format (MessagePack + zstd), thumbnails, slots, quick
# save and auto save (docs/SPEC.md section 8).
target_sources(
  vnmaki_engine
  PRIVATE persistent_store.cpp
          persistent_store.hpp
          save_file.cpp
          save_file.hpp
          saves.cpp
          saves.hpp
          thumbnail.cpp
          thumbnail.hpp)

# zstd has a CMake config in vcpkg (target zstd::libzstd) and, in newer releases, with the
# _shared or _static suffix; Debian/Ubuntu ship a plain library. zstd stays private to the engine.
find_package(zstd CONFIG QUIET)
foreach(vnmaki_zstd_candidate zstd::libzstd zstd::libzstd_shared zstd::libzstd_static)
  if(TARGET ${vnmaki_zstd_candidate})
    set(vnmaki_zstd_target ${vnmaki_zstd_candidate})
    break()
  endif()
endforeach()
if(vnmaki_zstd_target)
  target_link_libraries(vnmaki_engine PRIVATE ${vnmaki_zstd_target})
else()
  find_path(VNMAKI_ZSTD_INCLUDE_DIR zstd.h REQUIRED)
  find_library(VNMAKI_ZSTD_LIBRARY zstd REQUIRED)
  target_include_directories(vnmaki_engine SYSTEM PRIVATE "${VNMAKI_ZSTD_INCLUDE_DIR}")
  target_link_libraries(vnmaki_engine PRIVATE "${VNMAKI_ZSTD_LIBRARY}")
endif()
