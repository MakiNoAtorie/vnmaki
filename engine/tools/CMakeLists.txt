# Dev tools (docs/SPEC.md section 18). The Lua scan and the story graph need no ImGui and build in
# every preset, so `vnmaki graph` and `vnmaki check` work in a release build.
target_sources(
  vnmaki_engine
  PRIVATE check.cpp
          check.hpp
          observer_chain.hpp
          script_scan.cpp
          script_scan.hpp
          story_graph.cpp
          story_graph.hpp)

# The overlay, log console, Lua REPL, inspectors and hot reload are added only when VNMAKI_DEV_TOOLS is ON, so a release
# build has no overlay code and links no ImGui.
if(VNMAKI_DEV_TOOLS)
  target_sources(
    vnmaki_engine
    PRIVATE dap.cpp
            dap.hpp
            debugger.cpp
            debugger.hpp
            file_watcher.cpp
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

  # The debugger (task 36) speaks DAP in JSON and uses Lua's debug interface through sol2, with the
  # same sol2 settings as script_runtime.cpp.
  find_package(nlohmann_json CONFIG REQUIRED)
  target_link_libraries(vnmaki_engine PRIVATE nlohmann_json::nlohmann_json)
  set_property(
    SOURCE debugger.cpp TARGET_DIRECTORY vnmaki_engine
    APPEND
    PROPERTY COMPILE_DEFINITIONS SOL_ALL_SAFETIES_ON=1 SOL_PRINT_ERRORS=0)
endif()
