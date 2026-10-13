cmake_minimum_required(VERSION 3.25)

project(
  vnmaki
  VERSION 0.1.0
  DESCRIPTION "An open-source visual novel engine"
  HOMEPAGE_URL "https://github.com/MakiNoAtorie/vnmaki"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VNMAKI_BUILD_TESTS "Build the unit and CLI tests" ON)
option(VNMAKI_BUILD_FUZZERS "Build the libFuzzer targets (Clang only)" OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(VnmakiOptions)

add_subdirectory(engine)
add_subdirectory(cli)

enable_testing()

if(VNMAKI_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(VNMAKI_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()
