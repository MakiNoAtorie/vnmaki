# Platform module: window, input and the main loop (docs/SPEC.md section 6).
# Only files in this directory named sdl_* may include SDL headers; the test
# lint.no_sdl_outside_platform enforces it.
target_sources(vnmaki_engine PRIVATE geometry.hpp input.hpp main_loop.cpp main_loop.hpp
                                     platform.cpp platform.hpp viewport.cpp viewport.hpp)

# SDL3 backend. SDL is linked PRIVATE: no engine header outside this module
# exposes SDL types.
find_package(SDL3 CONFIG REQUIRED COMPONENTS SDL3)
target_sources(vnmaki_engine PRIVATE sdl_events.cpp sdl_events.hpp sdl_platform.cpp
                                     sdl_platform.hpp)
target_link_libraries(vnmaki_engine PRIVATE SDL3::SDL3)
