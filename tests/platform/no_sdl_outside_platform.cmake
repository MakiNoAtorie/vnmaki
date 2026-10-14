# Fails if a source file outside the SDL backend includes an SDL header.
#
# Usage: cmake -DSOURCE_DIR=<repo root> -P no_sdl_outside_platform.cmake
#
# SDL may be included only by engine/platform/sdl_* files (the backend). The
# rest of the engine and the CLI talk to the platform through its SDL-free
# headers, so another backend can replace SDL. To allow a new backend file,
# add a pattern to `allowed`.

set(allowed "^engine/platform/sdl_[^/]*$")
# The SDL GPU renderer backend (ticket 05).
list(APPEND allowed "^engine/render/sdl_[^/]*$")

file(
  GLOB_RECURSE sources
  RELATIVE "${SOURCE_DIR}"
  "${SOURCE_DIR}/engine/*.h" "${SOURCE_DIR}/engine/*.hpp" "${SOURCE_DIR}/engine/*.cpp"
  "${SOURCE_DIR}/cli/*.h" "${SOURCE_DIR}/cli/*.hpp" "${SOURCE_DIR}/cli/*.cpp")

set(offenders "")
foreach(source IN LISTS sources)
  set(is_allowed FALSE)
  foreach(pattern IN LISTS allowed)
    if(source MATCHES "${pattern}")
      set(is_allowed TRUE)
    endif()
  endforeach()
  if(is_allowed)
    continue()
  endif()
  file(STRINGS "${SOURCE_DIR}/${source}" sdl_includes REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]SDL")
  if(sdl_includes)
    list(APPEND offenders "${source}: ${sdl_includes}")
  endif()
endforeach()

if(offenders)
  list(JOIN offenders "\n  " report)
  message(FATAL_ERROR "SDL headers included outside engine/platform/sdl_*:\n  ${report}")
endif()
list(LENGTH sources checked)
message(STATUS "checked ${checked} files: no SDL includes outside the platform backend")
