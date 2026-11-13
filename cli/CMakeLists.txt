# The commands are a library so that the unit tests can call them (the launcher actions); the
# executable is only main.cpp.
add_library(vnmaki_cli STATIC)
target_link_libraries(vnmaki_cli PUBLIC vnmaki::engine PRIVATE vnmaki::options)
add_executable(vnmaki main.cpp)
target_link_libraries(vnmaki PRIVATE vnmaki_cli vnmaki::engine vnmaki::options)
target_sources(vnmaki_cli PRIVATE window_command.cpp window_command.hpp)
target_sources(vnmaki_cli PRIVATE image_view.cpp image_view.hpp)
target_sources(vnmaki_cli PRIVATE new_command.cpp new_command.hpp)
# Where `vnmaki new` finds templates/ when none is next to the executable: the source tree.
target_compile_definitions(vnmaki_cli
                           PRIVATE VNMAKI_SOURCE_TEMPLATES_DIR="${PROJECT_SOURCE_DIR}/templates")
target_sources(vnmaki_cli PRIVATE run_command.cpp run_command.hpp story_window.cpp
                                  story_window.hpp)
target_sources(vnmaki_cli PRIVATE graph_command.cpp graph_command.hpp)
target_sources(vnmaki_cli PRIVATE check_command.cpp check_command.hpp)
target_sources(vnmaki_cli PRIVATE debug_command.cpp debug_command.hpp)
target_sources(vnmaki_cli PRIVATE extract_command.cpp extract_command.hpp)
target_sources(vnmaki_cli PRIVATE import_command.cpp import_command.hpp)
target_sources(vnmaki_cli PRIVATE build_command.cpp build_command.hpp)
target_sources(vnmaki_cli PRIVATE launcher.cpp launcher.hpp)
# The launcher window draws with ImGui, so a release build has only the actions and no window.
if(VNMAKI_DEV_TOOLS)
  find_package(imgui CONFIG REQUIRED)
  target_sources(vnmaki_cli PRIVATE launcher_window.cpp)
  target_link_libraries(vnmaki_cli PRIVATE imgui::imgui)
endif()
