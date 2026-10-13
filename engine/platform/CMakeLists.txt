# Platform module: window, input and the main loop (docs/SPEC.md section 6).
# Only files in this directory named sdl_* may include SDL headers; the test
# lint.no_sdl_outside_platform enforces it.
target_sources(vnmaki_engine PRIVATE geometry.hpp input.hpp main_loop.cpp main_loop.hpp
                                     platform.cpp platform.hpp viewport.cpp viewport.hpp)
