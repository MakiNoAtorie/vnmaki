# Dev tools (docs/SPEC.md section 18). The Lua scan, the story graph, `check` and the profiler need
# no ImGui and build in every preset, so `vnmaki graph`, `vnmaki check` and `vnmaki run --profile`
# work in a release build.
target_sources(
  vnmaki_engine
  PRIVATE check.cpp
          check.hpp
          observer_chain.hpp
          profiler.cpp
          profiler.hpp
          script_scan.cpp
          script_scan.hpp
          story_graph.cpp
          story_graph.hpp)

# The overlay, log console, Lua REPL, inspectors and hot reload are added only when VNMAKI_DEV_TOOLS is ON, so a release
# build has no overlay code and links no ImGui.
if(VNMAKI_DEV_TOOLS)
  target_sources(
    vnmaki_engine
    PRIVATE file_watcher.cpp
            file_watcher.hpp
            hot_reload.cpp
            hot_reload.hpp
            imgui_backend.cpp
            imgui_backend.hpp
            inspect.hpp
            inspect_audio.cpp
            inspect_labels.cpp
            inspect_scene.cpp
            inspect_store.cpp
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
endif()
