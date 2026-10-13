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

# Render: image loading, the renderer interface, the SDL GPU backend (ticket 05).
target_sources(vnmaki_unit_tests PRIVATE unit/image_test.cpp)
add_subdirectory(render)
vnmaki_add_fixture_generator(
  render SCRIPT "${PROJECT_SOURCE_DIR}/tools/generators/render_fixtures.py"
  TEST_TARGETS vnmaki_unit_tests vnmaki_render_tests)
# Script runtime (ticket 06).
target_sources(vnmaki_unit_tests PRIVATE unit/headless_runner_test.cpp
                                         unit/script_sandbox_test.cpp
                                         unit/project_config_test.cpp)
vnmaki_add_fixture_generator(
  runtime SCRIPT "${PROJECT_SOURCE_DIR}/tools/generators/runtime_fixtures.py"
  TEST_TARGETS vnmaki_unit_tests)

# State store, step counting and replay (ticket 11).
target_sources(vnmaki_unit_tests PRIVATE unit/script_state_test.cpp
                                         unit/replay_property_test.cpp)
vnmaki_add_fixture_generator(
  replay SCRIPT "${PROJECT_SOURCE_DIR}/tools/generators/replay_fixtures.py"
  TEST_TARGETS vnmaki_unit_tests)
