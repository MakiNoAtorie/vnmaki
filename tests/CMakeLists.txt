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
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/generators/text_fixtures.py"
  TEST_TARGETS vnmaki_unit_tests vnmaki_render_tests)
# WebP images for the image decoder (ticket 31).
vnmaki_add_fixture_generator(
  webp SCRIPT "${PROJECT_SOURCE_DIR}/tools/generators/webp_fixtures.py"
  TEST_TARGETS vnmaki_unit_tests)
# Script runtime (ticket 06).
target_sources(vnmaki_unit_tests PRIVATE unit/headless_runner_test.cpp
                                         unit/script_sandbox_test.cpp
                                         unit/project_config_test.cpp)
vnmaki_add_fixture_generator(
  runtime SCRIPT "${PROJECT_SOURCE_DIR}/tools/generators/runtime_fixtures.py"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/generators/render_fixtures.py"
          "${PROJECT_SOURCE_DIR}/tools/generators/text_fixtures.py"
  TEST_TARGETS vnmaki_unit_tests)

# Text: layout, glyph atlas, typewriter reveal (ticket 07).
target_sources(vnmaki_unit_tests PRIVATE unit/text_test.cpp unit/markup_test.cpp
                                         unit/styled_text_test.cpp
                                         unit/script_text_test.cpp)
vnmaki_add_fixture_generator(
  text SCRIPT "${PROJECT_SOURCE_DIR}/tools/generators/text_fixtures.py"
  TEST_TARGETS vnmaki_unit_tests vnmaki_render_tests)

# State store, step counting and replay (ticket 11).
target_sources(vnmaki_unit_tests PRIVATE unit/script_state_test.cpp
                                         unit/replay_property_test.cpp)
vnmaki_add_fixture_generator(
  replay SCRIPT "${PROJECT_SOURCE_DIR}/tools/generators/replay_fixtures.py"
  TEST_TARGETS vnmaki_unit_tests)

# Scene graph, layers and displayables (ticket 09).
target_sources(vnmaki_unit_tests PRIVATE unit/scene_test.cpp)

# Animation: easings, tweens, the timeline and the script's wait (ticket 20).
target_sources(vnmaki_unit_tests PRIVATE unit/anim_test.cpp)

# Game shaders: the run-time compiler, the script API and uniform tweens (ticket 24).
target_sources(vnmaki_unit_tests PRIVATE unit/shader_test.cpp)
vnmaki_add_fixture_generator(
  shader SCRIPT "${PROJECT_SOURCE_DIR}/tools/generators/shader_fixtures.py"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/generators/render_fixtures.py"
          "${PROJECT_SOURCE_DIR}/tools/generators/text_fixtures.py"
  TEST_TARGETS vnmaki_render_tests)

# Transitions: the clock, options, the script API and blocking, and GPU goldens (ticket 21).
target_sources(vnmaki_unit_tests PRIVATE unit/transition_test.cpp)
vnmaki_add_fixture_generator(
  transition SCRIPT "${PROJECT_SOURCE_DIR}/tools/generators/transition_fixtures.py"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/generators/render_fixtures.py"
          "${PROJECT_SOURCE_DIR}/tools/generators/text_fixtures.py"
  TEST_TARGETS vnmaki_render_tests)

# Layered sprites: attributes, crossfade, snapshot, drawing order (ticket 22).
target_sources(vnmaki_unit_tests PRIVATE unit/layered_test.cpp)
vnmaki_add_fixture_generator(
  layered SCRIPT "${PROJECT_SOURCE_DIR}/tools/generators/layered_fixtures.py"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/generators/render_fixtures.py"
          "${PROJECT_SOURCE_DIR}/tools/generators/text_fixtures.py"
  TEST_TARGETS vnmaki_render_tests)

# Frame animation: sheets and sequences, frame at a time, wait, snapshot (ticket 27).
target_sources(vnmaki_unit_tests PRIVATE unit/frame_animation_test.cpp)
vnmaki_add_fixture_generator(
  frame_anim SCRIPT "${PROJECT_SOURCE_DIR}/tools/generators/frame_anim_fixtures.py"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/generators/render_fixtures.py"
          "${PROJECT_SOURCE_DIR}/tools/generators/text_fixtures.py"
  TEST_TARGETS vnmaki_render_tests)

# Camera: the view maths, shake noise, script calls, snapshot, reduced motion, GPU goldens
# (ticket 23).
target_sources(vnmaki_unit_tests PRIVATE unit/camera_test.cpp)
vnmaki_add_fixture_generator(
  camera SCRIPT "${PROJECT_SOURCE_DIR}/tools/generators/camera_fixtures.py"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/generators/render_fixtures.py"
          "${PROJECT_SOURCE_DIR}/tools/generators/text_fixtures.py"
  TEST_TARGETS vnmaki_render_tests)

# Lip sync: the voice level of the audio and the mouth of a layered sprite (ticket 26).
target_sources(vnmaki_unit_tests PRIVATE unit/lipsync_test.cpp)
vnmaki_add_fixture_generator(
  lipsync SCRIPT "${PROJECT_SOURCE_DIR}/tools/generators/lipsync_fixtures.py"
  TEST_TARGETS vnmaki_unit_tests)

# Audio: decoders, mixer state, script commands, snapshots (ticket 16).
target_sources(vnmaki_unit_tests PRIVATE unit/audio_decoder_test.cpp unit/audio_test.cpp)
vnmaki_add_fixture_generator(
  audio SCRIPT "${PROJECT_SOURCE_DIR}/tools/generators/audio_fixtures.py"
  TEST_TARGETS vnmaki_unit_tests)

# Say box: characters, the dialogue presenter and the say box view (ticket 08).
target_sources(vnmaki_unit_tests PRIVATE unit/character_test.cpp
                                         unit/dialogue_test.cpp)

# UI: nodes, Yoga layout, focus, the screen host, menus and call_screen (ticket 10).
target_sources(vnmaki_unit_tests PRIVATE unit/ui_test.cpp)

# UI styles, themes and the `input` node (ticket 30a).
target_sources(vnmaki_unit_tests PRIVATE unit/styles_test.cpp)

# Saves: file format, slots, quick save, auto save, on_load (ticket 12).
target_sources(vnmaki_unit_tests PRIVATE unit/saves_test.cpp)

# `vnmaki new` and the default template: the copy, the sample game played end to end, and the
# generator of its assets (ticket 19).
target_sources(vnmaki_unit_tests PRIVATE unit/new_project_test.cpp)
target_compile_definitions(vnmaki_unit_tests
                           PRIVATE VNMAKI_TEMPLATES_DIR="${PROJECT_SOURCE_DIR}/templates")
vnmaki_add_fixture_generator(
  template SCRIPT "${PROJECT_SOURCE_DIR}/tools/generators/template_assets.py"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/generators/render_fixtures.py"
          "${PROJECT_SOURCE_DIR}/tools/generators/shader_fixtures.py"
          "${PROJECT_SOURCE_DIR}/tools/generators/text_fixtures.py"
  TEST_TARGETS vnmaki_unit_tests)

# The presentation demo (ticket 28): GPU goldens of three frames of its label, drawn by
# tools/generators/presentation_fixtures.py from the template's pictures.
vnmaki_add_fixture_generator(
  presentation SCRIPT "${PROJECT_SOURCE_DIR}/tools/generators/presentation_fixtures.py"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/generators/render_fixtures.py"
          "${PROJECT_SOURCE_DIR}/tools/generators/shader_fixtures.py"
          "${PROJECT_SOURCE_DIR}/tools/generators/template_assets.py"
          "${PROJECT_SOURCE_DIR}/tools/generators/text_fixtures.py"
  TEST_TARGETS vnmaki_render_tests)

# Persistent store, seen text, skip and auto-advance (ticket 14).
target_sources(vnmaki_unit_tests PRIVATE unit/playback_test.cpp)

# Rollback: history, labels, menus, depth, the inputs (ticket 13).
target_sources(vnmaki_unit_tests PRIVATE unit/rollback_test.cpp)

# Preferences and the default screens: bind, toggle, slider, bar, grid, prefs, actions (ticket 18).
target_sources(vnmaki_unit_tests PRIVATE unit/screens_test.cpp)

# Backlog: entries from the step log, voice replay, the scrolling screen (ticket 15).
target_sources(vnmaki_unit_tests PRIVATE unit/backlog_test.cpp)

# Story graph: the Lua scan, the graph with its traces, DOT and CSV (ticket 34). The template is
# one more input. The fuzz target shares fuzz/script_scan_properties.hpp.
target_sources(vnmaki_unit_tests PRIVATE unit/script_scan_test.cpp unit/story_graph_test.cpp)
# The projects and expected CSV and DOT text of the `vnmaki graph` CLI tests.
vnmaki_add_fixture_generator(
  graph SCRIPT "${PROJECT_SOURCE_DIR}/tools/generators/graph_fixtures.py"
  TEST_TARGETS vnmaki_unit_tests)

# Video: WebM, AV1 and Opus, the player and its clocks, `movie` and the movie displayable (ticket
# 25). clip.webm is the one committed binary fixture; its README has the ffmpeg command.
target_sources(vnmaki_unit_tests PRIVATE unit/video_test.cpp)
target_compile_definitions(
  vnmaki_unit_tests PRIVATE VNMAKI_VIDEO_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/video")

# pause, NVL mode and the gallery (ticket 29).
target_sources(vnmaki_unit_tests PRIVATE unit/pause_nvl_gallery_test.cpp)

# Dev tools: the REPL and the log buffer (ticket 30). They exist only in builds with the tools.
if(VNMAKI_DEV_TOOLS)
  target_sources(vnmaki_unit_tests PRIVATE unit/repl_test.cpp)
endif()

# Dev tools: the row functions of the inspectors, on the default template (ticket 31).
if(VNMAKI_DEV_TOOLS)
  target_sources(vnmaki_unit_tests PRIVATE unit/inspect_test.cpp)
endif()
