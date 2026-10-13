# The engine is one static library. Each module directory under engine/ adds
# its sources to it with target_sources().
add_library(vnmaki_engine STATIC)
add_library(vnmaki::engine ALIAS vnmaki_engine)

# Engine headers are included by their path from the repository root, e.g.
# #include "engine/core/version.hpp".
target_include_directories(vnmaki_engine PUBLIC "${PROJECT_SOURCE_DIR}")
target_compile_features(vnmaki_engine PUBLIC cxx_std_20)
target_link_libraries(vnmaki_engine PRIVATE vnmaki::options)

add_subdirectory(core)
add_subdirectory(jobs)
add_subdirectory(vfs)
add_subdirectory(platform)
add_subdirectory(render)
add_subdirectory(runtime)
add_subdirectory(text)
