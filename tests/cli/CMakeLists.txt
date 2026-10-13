# Black-box tests of the vnmaki executable: each runs it as a process.
function(vnmaki_add_cli_test name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "NO_STDOUT" "EXIT;STDOUT_LINE;STDERR_REGEX"
                        "ARGS")
  # Keep a multi-argument ARGS list in the one -DARGS=... argument.
  string(REPLACE ";" "\\;" arg_ARGS "${arg_ARGS}")
  set(script_args -DEXE=$<TARGET_FILE:vnmaki> "-DARGS=${arg_ARGS}"
                  -DEXPECTED_EXIT=${arg_EXIT})
  if(arg_NO_STDOUT)
    list(APPEND script_args -DEXPECTED_NO_STDOUT=ON)
  elseif(DEFINED arg_STDOUT_LINE)
    list(APPEND script_args "-DEXPECTED_STDOUT_LINE=${arg_STDOUT_LINE}")
  endif()
  if(DEFINED arg_STDERR_REGEX)
    list(APPEND script_args "-DEXPECTED_STDERR_REGEX=${arg_STDERR_REGEX}")
  endif()
  add_test(NAME cli.${name} COMMAND ${CMAKE_COMMAND} ${script_args} -P
                                    "${CMAKE_CURRENT_SOURCE_DIR}/run_cli.cmake")
endfunction()

vnmaki_add_cli_test(
  version
  ARGS --version
  EXIT 0
  STDOUT_LINE "vnmaki ${PROJECT_VERSION}")

vnmaki_add_cli_test(
  unknown_option_is_a_usage_error
  ARGS --frobnicate
  EXIT 2
  NO_STDOUT
  STDERR_REGEX "unknown option '--frobnicate'.*usage: vnmaki")

vnmaki_add_cli_test(
  no_arguments_prints_usage
  EXIT 2
  NO_STDOUT
  STDERR_REGEX "^usage: vnmaki")

# vnmaki window (ticket 04): opens an empty window and runs the main loop.
vnmaki_add_cli_test(
  window_opens_and_runs_some_frames
  ARGS window --frames 3
  EXIT 0
  NO_STDOUT)

vnmaki_add_cli_test(
  window_reports_a_video_driver_that_cannot_start
  ARGS window --frames 1
  EXIT 1
  NO_STDOUT
  STDERR_REGEX "^vnmaki window: cannot start SDL video")
set_tests_properties(cli.window_reports_a_video_driver_that_cannot_start
                     PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=no-such-driver")

vnmaki_add_cli_test(
  window_frames_must_be_a_number
  ARGS window --frames many
  EXIT 2
  NO_STDOUT
  STDERR_REGEX "vnmaki window: --frames needs a whole number, not 'many'")

vnmaki_add_cli_test(
  window_rejects_unknown_options
  ARGS window --frobnicate
  EXIT 2
  NO_STDOUT
  STDERR_REGEX "vnmaki window: unknown option '--frobnicate'.*usage: .*vnmaki window")

# A bug in the frame limit must fail the test, not hang CI with an open window.
set_tests_properties(cli.window_opens_and_runs_some_frames PROPERTIES TIMEOUT 60)
# Linux CI has no display: open the window with SDL's offscreen driver there.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set_tests_properties(cli.window_opens_and_runs_some_frames
                       PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen")
endif()

# vnmaki run (ticket 06), on the projects tools/generators/runtime_fixtures.py
# writes (registered in tests/CMakeLists.txt).
set(vnmaki_run_fixtures "${VNMAKI_FIXTURES_DIR}/runtime")

# Like vnmaki_add_cli_test, but stdout must equal the file STDOUT_FILE.
function(vnmaki_add_cli_stdout_file_test name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "EXIT;STDOUT_FILE;STDERR_REGEX" "ARGS")
  string(REPLACE ";" "\\;" arg_ARGS "${arg_ARGS}")
  set(script_args -DEXE=$<TARGET_FILE:vnmaki> "-DARGS=${arg_ARGS}"
                  -DEXPECTED_EXIT=${arg_EXIT} "-DEXPECTED_STDOUT_FILE=${arg_STDOUT_FILE}")
  if(DEFINED arg_STDERR_REGEX)
    list(APPEND script_args "-DEXPECTED_STDERR_REGEX=${arg_STDERR_REGEX}")
  endif()
  add_test(NAME cli.${name} COMMAND ${CMAKE_COMMAND} ${script_args} -P
                                    "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_stdout_file.cmake")
endfunction()

vnmaki_add_cli_stdout_file_test(
  run_headless_prints_the_transcript
  ARGS run "${vnmaki_run_fixtures}/demo" --headless
  EXIT 0
  STDOUT_FILE "${vnmaki_run_fixtures}/demo.transcript")

# With no project argument, vnmaki run uses the current directory.
add_test(NAME cli.run_headless_defaults_to_the_current_directory
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:vnmaki> "-DARGS=run;--headless"
                 -DEXPECTED_EXIT=0
                 "-DEXPECTED_STDOUT_FILE=${vnmaki_run_fixtures}/demo.transcript" -P
                 "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_stdout_file.cmake"
         WORKING_DIRECTORY "${vnmaki_run_fixtures}/demo")

vnmaki_add_cli_test(
  run_headless_reports_script_errors
  ARGS run "${vnmaki_run_fixtures}/broken" --headless
  EXIT 1
  STDERR_REGEX "vnmaki: scripts/start.lua:3: unknown label 'nowhere'")

vnmaki_add_cli_test(
  run_headless_stops_at_the_step_limit
  ARGS run "${vnmaki_run_fixtures}/endless" --headless --max-steps 3
  EXIT 1
  STDERR_REGEX "vnmaki: stopped after 3 steps")

vnmaki_add_cli_test(
  run_reports_an_invalid_project_toml
  ARGS run "${vnmaki_run_fixtures}/bad_config" --headless
  EXIT 1
  NO_STDOUT
  STDERR_REGEX "vnmaki: project.toml: missing required key 'title'")

vnmaki_add_cli_test(
  run_reports_a_missing_project_toml
  ARGS run "${vnmaki_run_fixtures}/no_config" --headless
  EXIT 1
  NO_STDOUT
  STDERR_REGEX "vnmaki: project.toml: not found")

vnmaki_add_cli_test(
  run_reports_a_missing_project_folder
  ARGS run "${vnmaki_run_fixtures}/does_not_exist" --headless
  EXIT 1
  NO_STDOUT
  STDERR_REGEX "vnmaki: .*does_not_exist.* is not a directory")

# Only the headless runner exists so far; the windowed one comes later.
vnmaki_add_cli_test(
  run_without_headless_is_a_usage_error
  ARGS run "${vnmaki_run_fixtures}/demo"
  EXIT 2
  NO_STDOUT
  STDERR_REGEX "vnmaki run: only --headless is available")

vnmaki_add_cli_test(
  run_rejects_a_bad_step_limit
  ARGS run "${vnmaki_run_fixtures}/demo" --headless --max-steps nope
  EXIT 2
  NO_STDOUT
  STDERR_REGEX "vnmaki run: --max-steps needs a positive whole number")
