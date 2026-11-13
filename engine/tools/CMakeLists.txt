# Dev tools that need no ImGui (ticket 34): the Lua scan and the story graph. They build in every
# preset, so `vnmaki graph` and the overlay panel can both use them.
target_sources(
  vnmaki_engine
  PRIVATE script_scan.cpp
          script_scan.hpp
          story_graph.cpp
          story_graph.hpp)
