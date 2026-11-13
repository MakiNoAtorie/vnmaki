# UI module (ticket 10): the node tree, Yoga layout, focus, the screen host, the screen presenter
# and the renderer (docs/SPEC.md section 15).
target_sources(
  vnmaki_engine
  PRIVATE backlog_screen.cpp
          backlog_screen.hpp
          playback_indicator.cpp
          playback_indicator.hpp
          preferences.cpp
          preferences.hpp
          screen_actions.cpp
          screen_actions.hpp
          screen_presenter.cpp
          screen_presenter.hpp
          self_voicing.cpp
          self_voicing.hpp
          ui_build.cpp
          ui_build.hpp
          ui_focus.cpp
          ui_focus.hpp
          ui_host.cpp
          ui_host.hpp
          ui_layout.cpp
          ui_layout.hpp
          ui_node.cpp
          ui_node.hpp
          ui_renderer.cpp
          ui_renderer.hpp)

# Yoga has a CMake config in vcpkg and in the system prefix. It is private to the engine: no
# engine header includes it.
find_package(yoga CONFIG REQUIRED)
target_link_libraries(vnmaki_engine PRIVATE yoga::yogacore)
