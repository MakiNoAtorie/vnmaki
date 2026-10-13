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
# Make doctest include the real <ostream> instead of forward-declaring it.
# Without this, MSVC fails to stringify std::string_view in assertions.
target_compile_definitions(vnmaki_unit_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
target_sources(vnmaki_unit_tests PRIVATE unit/job_system_test.cpp)
doctest_discover_tests(vnmaki_unit_tests TEST_PREFIX "unit.")

# VFS (ticket 02).
target_sources(vnmaki_unit_tests PRIVATE unit/vfs_path_test.cpp unit/vfs_test.cpp)
include(VnmakiFixtures)
vnmaki_add_fixture_generator(
  vfs SCRIPT "${PROJECT_SOURCE_DIR}/tools/generators/vfs_fixtures.py"
  TEST_TARGETS vnmaki_unit_tests)

add_subdirectory(cli)

# Platform: window, input, main loop, virtual resolution (ticket 04).
add_subdirectory(platform)
