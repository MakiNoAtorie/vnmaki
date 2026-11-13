# Dev tools: the ImGui overlay, log console, Lua REPL and hot reload (docs/SPEC.md section 18).
# Added only when VNMAKI_DEV_TOOLS is ON, so a release build has no tool code and links no ImGui or
# efsw.
target_sources(
  vnmaki_engine
  PRIVATE file_watcher.cpp
          file_watcher.hpp
          hot_reload.cpp
          hot_reload.hpp
          imgui_backend.cpp
          imgui_backend.hpp
          log_buffer.cpp
          log_buffer.hpp
          overlay.cpp
          overlay.hpp
          repl.cpp
          repl.hpp)

# The ImGui renderer backend draws through SDL GPU, so only imgui_backend.cpp includes SDL
# (lint.no_sdl_outside_platform). The ImGui input comes from InputEvents, not from SDL events.
find_package(imgui CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED COMPONENTS SDL3)
find_package(efsw CONFIG REQUIRED)
target_link_libraries(vnmaki_engine PRIVATE imgui::imgui SDL3::SDL3 efsw::efsw)
