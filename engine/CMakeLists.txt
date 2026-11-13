# The engine is one static library. Each module directory under engine/ adds
# its sources to it with target_sources().
add_library(vnmaki_engine STATIC)
add_library(vnmaki::engine ALIAS vnmaki_engine)

# Engine headers are included by their path from the repository root, e.g.
# #include "engine/core/version.hpp".
target_include_directories(vnmaki_engine PUBLIC "${PROJECT_SOURCE_DIR}")
target_compile_features(vnmaki_engine PUBLIC cxx_std_20)
target_link_libraries(vnmaki_engine PRIVATE vnmaki::options)

add_subdirectory(anim)
add_subdirectory(core)
add_subdirectory(dialogue)
add_subdirectory(jobs)
add_subdirectory(vfs)
add_subdirectory(platform)
add_subdirectory(render)
add_subdirectory(runtime)
add_subdirectory(saves)
add_subdirectory(scene)
add_subdirectory(text)
add_subdirectory(tools)
add_subdirectory(ui)
add_subdirectory(audio)
add_subdirectory(video)

# Dev tools: the overlay in engine/tools/ is compiled only for them (engine/tools/CMakeLists.txt).
# `#if VNMAKI_DEV_TOOLS` guards the callers (cli/story_window.cpp). The scan and the graph there
# build in every preset.
if(VNMAKI_DEV_TOOLS)
  target_compile_definitions(vnmaki_engine PUBLIC VNMAKI_DEV_TOOLS=1)
else()
  target_compile_definitions(vnmaki_engine PUBLIC VNMAKI_DEV_TOOLS=0)
endif()
