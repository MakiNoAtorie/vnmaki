# Video module (ticket 25): WebM (libwebm) with AV1 (dav1d) and Opus, as textures (docs/SPEC.md
# sections 14, 4 and 5).
target_sources(
  vnmaki_engine
  PRIVATE movie.cpp
          movie.hpp
          movie_audio.hpp
          movie_player.cpp
          movie_player.hpp
          movie_presenter.cpp
          movie_presenter.hpp
          video_decoder.cpp
          video_decoder.hpp)

# dav1d has pkg-config files only (vcpkg and Debian), so find it by name.
find_path(VNMAKI_DAV1D_INCLUDE_DIR dav1d/dav1d.h REQUIRED)
find_library(VNMAKI_DAV1D_LIBRARY NAMES dav1d libdav1d REQUIRED)
target_include_directories(vnmaki_engine SYSTEM PRIVATE "${VNMAKI_DAV1D_INCLUDE_DIR}")
target_link_libraries(vnmaki_engine PRIVATE "${VNMAKI_DAV1D_LIBRARY}")

# libwebm's mkvparser (<mkvparser/mkvparser.h>). vcpkg installs the headers in include/webm and
# Debian has plain files, so look in a webm directory too.
find_path(VNMAKI_WEBM_INCLUDE_DIR mkvparser/mkvparser.h PATH_SUFFIXES webm REQUIRED)
find_library(VNMAKI_WEBM_LIBRARY NAMES webm libwebm REQUIRED)
target_include_directories(vnmaki_engine SYSTEM PRIVATE "${VNMAKI_WEBM_INCLUDE_DIR}")
target_link_libraries(vnmaki_engine PRIVATE "${VNMAKI_WEBM_LIBRARY}")

# movie.cpp writes the movie's Opus packets as an Ogg stream, so the audio decoder can read them.
# The audio module has found libogg's library in the Vorbis fallback; vcpkg has a CMake config.
find_package(Ogg CONFIG QUIET)
if(TARGET Ogg::ogg)
  target_link_libraries(vnmaki_engine PRIVATE Ogg::ogg)
else()
  find_path(VNMAKI_OGG_INCLUDE_DIR ogg/ogg.h REQUIRED)
  find_library(VNMAKI_OGG_LIB NAMES ogg REQUIRED)
  target_include_directories(vnmaki_engine SYSTEM PRIVATE "${VNMAKI_OGG_INCLUDE_DIR}")
  target_link_libraries(vnmaki_engine PRIVATE "${VNMAKI_OGG_LIB}")
endif()
