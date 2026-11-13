# 3D scenes (docs/SPEC.md section 12): glTF models, a camera, lights and the displayable that draws
# them (ticket 46).
target_sources(vnmaki_engine PRIVATE cgltf_impl.cpp math3d.cpp math3d.hpp model.cpp model.hpp
                                          scene3d.cpp scene3d.hpp)

# cgltf is one header. vcpkg installs it at the top of its include directory.
find_path(VNMAKI_CGLTF_INCLUDE_DIR cgltf.h REQUIRED)
target_include_directories(vnmaki_engine SYSTEM PRIVATE "${VNMAKI_CGLTF_INCLUDE_DIR}")
# The implementation is third-party code: compile it without the project's warnings, and keep
# clang-tidy off it.
if(MSVC)
  set(vnmaki_cgltf_flags /W0)
else()
  set(vnmaki_cgltf_flags -w)
endif()
set_source_files_properties(cgltf_impl.cpp TARGET_DIRECTORY vnmaki_engine PROPERTIES
                            COMPILE_OPTIONS "${vnmaki_cgltf_flags}" SKIP_LINTING ON)
