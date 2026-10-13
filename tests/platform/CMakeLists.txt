# Platform tests get their own executable so that only it links SDL and runs
# with SDL's offscreen video driver on Linux (CI runners have no display).
add_executable(vnmaki_platform_tests ../unit/main.cpp viewport_test.cpp)
target_link_libraries(vnmaki_platform_tests PRIVATE vnmaki::engine vnmaki::options
                                                    doctest::doctest)
target_compile_definitions(vnmaki_platform_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
doctest_discover_tests(vnmaki_platform_tests TEST_PREFIX "unit.")
