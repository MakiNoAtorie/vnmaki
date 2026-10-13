# Project-wide build guardrails (docs/SPEC.md section 5).
#
# Every first-party target links `vnmaki::options`, which carries the warning
# flags, the debug standard-library hardening and the sanitizer flags.
# Third-party code never links it, so its warnings stay out of our -Werror.

# Use ccache when it is installed. It is optional.
find_program(VNMAKI_CCACHE_PROGRAM NAMES ccache)
if(VNMAKI_CCACHE_PROGRAM AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
  set(CMAKE_CXX_COMPILER_LAUNCHER "${VNMAKI_CCACHE_PROGRAM}")
endif()

set(VNMAKI_SANITIZERS
    ""
    CACHE STRING "Sanitizers to enable: a list of address, undefined, thread")

add_library(vnmaki_options INTERFACE)
add_library(vnmaki::options ALIAS vnmaki_options)

if(MSVC)
  target_compile_options(vnmaki_options INTERFACE /W4 /WX /permissive- /utf-8
                                                  /Zc:__cplusplus)
  # Debug builds already use the checked STL (_ITERATOR_DEBUG_LEVEL=2).
else()
  target_compile_options(
    vnmaki_options
    INTERFACE -Wall
              -Wextra
              -Wpedantic
              -Wconversion
              -Wshadow
              -Werror)
  # Hardened standard library in debug builds. Each macro is ignored by the
  # standard library it does not belong to.
  target_compile_definitions(
    vnmaki_options
    INTERFACE
      $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>
      $<$<CONFIG:Debug>:_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG>)
endif()

if(VNMAKI_SANITIZERS)
  if("thread" IN_LIST VNMAKI_SANITIZERS AND "address" IN_LIST VNMAKI_SANITIZERS)
    message(FATAL_ERROR "ThreadSanitizer cannot be combined with AddressSanitizer")
  endif()
  if(MSVC)
    if(NOT VNMAKI_SANITIZERS STREQUAL "address")
      message(FATAL_ERROR "MSVC supports only VNMAKI_SANITIZERS=address")
    endif()
    target_compile_options(vnmaki_options INTERFACE /fsanitize=address)
  else()
    list(JOIN VNMAKI_SANITIZERS "," vnmaki_sanitizer_list)
    set(vnmaki_sanitizer_flags -fsanitize=${vnmaki_sanitizer_list}
                               -fno-omit-frame-pointer)
    if("undefined" IN_LIST VNMAKI_SANITIZERS)
      # Make every UBSan finding fatal so tests fail on it.
      list(APPEND vnmaki_sanitizer_flags -fno-sanitize-recover=all)
    endif()
    target_compile_options(vnmaki_options INTERFACE ${vnmaki_sanitizer_flags})
    target_link_options(vnmaki_options INTERFACE ${vnmaki_sanitizer_flags})
  endif()
endif()

if(VNMAKI_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "VNMAKI_BUILD_FUZZERS needs LLVM Clang (libFuzzer); "
                        "the compiler is ${CMAKE_CXX_COMPILER_ID}")
  endif()
  # Instrument all first-party code for coverage-guided fuzzing. Linking with
  # the same flag pulls in the coverage runtime, so the CLI and tests still
  # link. Only the fuzz targets link the libFuzzer driver (-fsanitize=fuzzer).
  target_compile_options(vnmaki_options INTERFACE -fsanitize=fuzzer-no-link)
  target_link_options(vnmaki_options INTERFACE -fsanitize=fuzzer-no-link)
endif()
