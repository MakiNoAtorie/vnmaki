# Platform tests get their own executable so that only it links SDL and runs
# with SDL's offscreen video driver on Linux (CI runners have no display).
find_package(SDL3 CONFIG REQUIRED COMPONENTS SDL3)

add_executable(
  vnmaki_platform_tests
  ../unit/main.cpp headless_platform_test.cpp main_loop_test.cpp sdl_events_test.cpp
  sdl_platform_test.cpp viewport_test.cpp)
target_link_libraries(vnmaki_platform_tests PRIVATE vnmaki::engine vnmaki::options
                                                    doctest::doctest SDL3::SDL3)
target_compile_definitions(vnmaki_platform_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)

# Windows and macOS runners have a desktop, so the window tests open real
# windows there. Linux CI has no display: use the offscreen driver.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(vnmaki_platform_test_env "SDL_VIDEO_DRIVER=offscreen")
endif()
doctest_discover_tests(vnmaki_platform_tests TEST_PREFIX "unit." PROPERTIES ENVIRONMENT
                       "${vnmaki_platform_test_env}")

# Engine code outside the platform backend never includes SDL headers.
add_test(NAME lint.no_sdl_outside_platform
         COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P
                 "${CMAKE_CURRENT_SOURCE_DIR}/no_sdl_outside_platform.cmake")
