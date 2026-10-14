# Audio module (ticket 16): decoders, the channel mixer and the output device (docs/SPEC.md
# section 14).
target_sources(vnmaki_engine PRIVATE audio.cpp audio.hpp audio_decoder.cpp audio_decoder.hpp)

# miniaudio is one header: the device, the FLAC and WAV decoders and the sample-rate converter.
# vcpkg installs it at the top of its include directory, Debian under miniaudio/.
find_path(VNMAKI_MINIAUDIO_INCLUDE_DIR miniaudio.h PATH_SUFFIXES miniaudio REQUIRED)
target_include_directories(vnmaki_engine SYSTEM PRIVATE "${VNMAKI_MINIAUDIO_INCLUDE_DIR}")
target_link_libraries(vnmaki_engine PRIVATE ${CMAKE_DL_LIBS})

# opusfile and vorbisfile have CMake configs in vcpkg only; Debian/Ubuntu ship plain libraries.
# <opusfile.h> includes the Opus headers without a directory, so its own directory is the
# include path.
find_package(OpusFile CONFIG QUIET)
if(TARGET OpusFile::opusfile)
  target_link_libraries(vnmaki_engine PRIVATE OpusFile::opusfile)
else()
  find_path(VNMAKI_OPUSFILE_INCLUDE_DIR opusfile.h PATH_SUFFIXES opus REQUIRED)
  find_library(VNMAKI_OPUSFILE_LIBRARY opusfile REQUIRED)
  find_library(VNMAKI_OPUS_LIBRARY opus REQUIRED)
  target_include_directories(vnmaki_engine SYSTEM PRIVATE "${VNMAKI_OPUSFILE_INCLUDE_DIR}")
  target_link_libraries(vnmaki_engine PRIVATE "${VNMAKI_OPUSFILE_LIBRARY}"
                                              "${VNMAKI_OPUS_LIBRARY}")
endif()
find_package(Vorbis CONFIG QUIET)
if(TARGET Vorbis::vorbisfile)
  target_link_libraries(vnmaki_engine PRIVATE Vorbis::vorbisfile)
else()
  find_path(VNMAKI_VORBIS_INCLUDE_DIR vorbis/vorbisfile.h REQUIRED)
  find_library(VNMAKI_VORBISFILE_LIBRARY vorbisfile REQUIRED)
  find_library(VNMAKI_VORBIS_LIBRARY vorbis REQUIRED)
  find_library(VNMAKI_OGG_LIBRARY ogg REQUIRED)
  target_include_directories(vnmaki_engine SYSTEM PRIVATE "${VNMAKI_VORBIS_INCLUDE_DIR}")
  target_link_libraries(
    vnmaki_engine PRIVATE "${VNMAKI_VORBISFILE_LIBRARY}" "${VNMAKI_VORBIS_LIBRARY}"
                          "${VNMAKI_OGG_LIBRARY}")
endif()

# Only the parts of miniaudio the engine uses. Without threads (VNMAKI_ENABLE_THREADS=OFF) there
# is no device either: the mixer is then stepped by Audio::update().
set(vnmaki_miniaudio_definitions MA_NO_ENCODING MA_NO_MP3 MA_NO_GENERATION MA_NO_ENGINE
                                 MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER)
if(VNMAKI_ENABLE_THREADS)
  set(vnmaki_audio_device 1)
else()
  set(vnmaki_audio_device 0)
  list(APPEND vnmaki_miniaudio_definitions MA_NO_DEVICE_IO MA_NO_THREADING)
endif()
# Every file that includes miniaudio.h must see the same definitions.
target_sources(vnmaki_engine PRIVATE miniaudio_impl.cpp)
set_property(
  SOURCE audio.cpp audio_decoder.cpp miniaudio_impl.cpp TARGET_DIRECTORY vnmaki_engine
  APPEND
  PROPERTY COMPILE_DEFINITIONS ${vnmaki_miniaudio_definitions}
           VNMAKI_AUDIO_DEVICE=${vnmaki_audio_device})

# The implementation is third-party code: compile it without the project's warnings and keep
# clang-tidy off it. It still gets the sanitizer and fuzzing instrumentation from
# vnmaki::options, so the audio fuzz target covers dr_flac and dr_wav themselves.
if(MSVC)
  set(vnmaki_miniaudio_flags /W0)
else()
  set(vnmaki_miniaudio_flags -w)
endif()
set_source_files_properties(
  miniaudio_impl.cpp TARGET_DIRECTORY vnmaki_engine PROPERTIES COMPILE_OPTIONS
                                                               "${vnmaki_miniaudio_flags}"
                                                               SKIP_LINTING ON)
