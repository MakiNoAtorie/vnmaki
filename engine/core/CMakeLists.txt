target_sources(vnmaki_engine PRIVATE version.cpp version.hpp)

# project() in the top-level CMakeLists.txt is the single source of the version.
set_property(
  SOURCE version.cpp TARGET_DIRECTORY vnmaki_engine
  APPEND
  PROPERTY COMPILE_DEFINITIONS
           VNMAKI_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
           VNMAKI_VERSION_MINOR=${PROJECT_VERSION_MINOR}
           VNMAKI_VERSION_PATCH=${PROJECT_VERSION_PATCH}
           VNMAKI_VERSION_STRING="${PROJECT_VERSION}")
