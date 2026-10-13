find_package(doctest 2.4 CONFIG REQUIRED)
include(doctest)

add_executable(vnmaki_unit_tests unit/main.cpp unit/version_test.cpp)
target_link_libraries(vnmaki_unit_tests PRIVATE vnmaki::engine vnmaki::options
                                                doctest::doctest)
# The version declared in project() is the source of truth the tests check
# the engine against.
target_compile_definitions(
  vnmaki_unit_tests PRIVATE VNMAKI_EXPECTED_VERSION="${PROJECT_VERSION}"
                            VNMAKI_EXPECTED_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
                            VNMAKI_EXPECTED_VERSION_MINOR=${PROJECT_VERSION_MINOR}
                            VNMAKI_EXPECTED_VERSION_PATCH=${PROJECT_VERSION_PATCH})
doctest_discover_tests(vnmaki_unit_tests TEST_PREFIX "unit.")

add_subdirectory(cli)
