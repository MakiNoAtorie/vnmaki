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
# The dev overlay and the other tools of docs/SPEC.md section 18 (engine/tools/, Dear ImGui). On
# for debug builds, off for release builds: those compile none of it and link no ImGui.
if(CMAKE_BUILD_TYPE MATCHES "^(Release|MinSizeRel|RelWithDebInfo)$")
  set(vnmaki_dev_tools_default OFF)
else()
  set(vnmaki_dev_tools_default ON)
endif()
option(VNMAKI_DEV_TOOLS "Build the dev tools (overlay, console, REPL)" ${vnmaki_dev_tools_default})
# SDL_shadercross: game shaders compile when they load, and `vnmaki import` precompiles them
# (docs/SPEC.md section 19). A release build links none of it and loads the precompiled shaders.
option(VNMAKI_SHADER_COMPILER "Link the run-time shader compiler (SDL_shadercross)"
       ${vnmaki_dev_tools_default})

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(VnmakiOptions)
include(VnmakiPlugins)

add_subdirectory(engine)
add_subdirectory(plugins)
add_subdirectory(cli)

enable_testing()

if(VNMAKI_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(VNMAKI_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()
