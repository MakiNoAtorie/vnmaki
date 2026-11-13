# Black-box tests of the vnmaki executable: each runs it as a process.

# `vnmaki run` writes saves and `persistent.vnpersist`. Give each such test a folder of its own in
# the build tree (--data-dir), so that no test touches the user's data or shares state with
# another. The runner scripts empty it first (-DDATA_DIR). Sets <define_var> to that option.
function(vnmaki_isolate_run_data name args_var define_var)
  set(args ${${args_var}})
  set(define "")
  if(args MATCHES "^(run|debug)(;|$)")
    set(directory "${CMAKE_CURRENT_BINARY_DIR}/data/${name}")
    list(APPEND args --data-dir "${directory}")
    set(define "-DDATA_DIR=${directory}")
  endif()
  set(${args_var} "${args}" PARENT_SCOPE)
  set(${define_var} "${define}" PARENT_SCOPE)
endfunction()

function(vnmaki_add_cli_test name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "NO_STDOUT" "EXIT;STDOUT_LINE;STDERR_REGEX"
                        "ARGS;EXPECTED_FILES")
  vnmaki_isolate_run_data(${name} arg_ARGS data_define)
  # Keep a multi-argument ARGS list in the one -DARGS=... argument.
  string(REPLACE ";" "\\;" arg_ARGS "${arg_ARGS}")
  string(REPLACE ";" "\\;" expected_files "${arg_EXPECTED_FILES}")
  set(script_args -DEXE=$<TARGET_FILE:vnmaki> "-DARGS=${arg_ARGS}"
                  -DEXPECTED_EXIT=${arg_EXIT} ${data_define} "-DEXPECTED_FILES=${expected_files}")
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

# vnmaki window --image (ticket 05): draws a PNG through the VFS, the job system and the GPU.
set(vnmaki_cli_images "${VNMAKI_FIXTURES_DIR}/render/images")
vnmaki_add_cli_test(
  window_shows_an_image
  ARGS window --frames 3 --image "${vnmaki_cli_images}/checker_4x4.png"
  EXIT 0
  NO_STDOUT)
vnmaki_add_cli_test(
  window_reports_a_missing_image
  ARGS window --frames 1 --image "${vnmaki_cli_images}/no_such_image.png"
  EXIT 1
  NO_STDOUT
  STDERR_REGEX "^vnmaki window: no image file at '.*no_such_image.png'")
vnmaki_add_cli_test(
  window_reports_an_image_it_cannot_decode
  ARGS window --frames 1 --image "${vnmaki_cli_images}/truncated.png"
  EXIT 1
  NO_STDOUT
  STDERR_REGEX "^vnmaki window: cannot decode image 'truncated.png'")
set(vnmaki_gpu_cli_tests cli.window_shows_an_image cli.window_reports_an_image_it_cannot_decode)
set_tests_properties(cli.window_reports_a_missing_image ${vnmaki_gpu_cli_tests} PROPERTIES TIMEOUT
                                                                                           60)
set(vnmaki_gpu_cli_env "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND vnmaki_gpu_cli_env "SDL_VIDEO_DRIVER=offscreen")
endif()
# GPU drivers unloaded at exit make LeakSanitizer report false leaks (see
# tests/render/CMakeLists.txt).
if("address" IN_LIST VNMAKI_SANITIZERS)
  list(APPEND vnmaki_gpu_cli_env "ASAN_OPTIONS=detect_leaks=0")
endif()
# Races inside the GPU driver's own threads are not the engine's.
if("thread" IN_LIST VNMAKI_SANITIZERS)
  list(APPEND vnmaki_gpu_cli_env
       "TSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tests/render/gpu_driver.tsan.supp")
endif()
if(vnmaki_gpu_cli_env)
  set_tests_properties(cli.window_reports_a_missing_image ${vnmaki_gpu_cli_tests}
                       PROPERTIES ENVIRONMENT "${vnmaki_gpu_cli_env}")
endif()
# Without any usable GPU device (not even a software one) these are skipped, as in
# tests/render/CMakeLists.txt, unless VNMAKI_REQUIRE_GPU=1 was set when configuring.
if(NOT "$ENV{VNMAKI_REQUIRE_GPU}" STREQUAL "1")
  set_tests_properties(${vnmaki_gpu_cli_tests} PROPERTIES SKIP_REGULAR_EXPRESSION
                                                          "cannot create a GPU device")
endif()
# vnmaki run (ticket 06), on the projects tools/generators/runtime_fixtures.py
# writes (registered in tests/CMakeLists.txt).
set(vnmaki_run_fixtures "${VNMAKI_FIXTURES_DIR}/runtime")

# Like vnmaki_add_cli_test, but stdout must equal the file STDOUT_FILE.
function(vnmaki_add_cli_stdout_file_test name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "EXIT;STDOUT_FILE;STDERR_REGEX"
                        "ARGS;EXPECTED_FILES")
  vnmaki_isolate_run_data(${name} arg_ARGS data_define)
  string(REPLACE ";" "\\;" arg_ARGS "${arg_ARGS}")
  string(REPLACE ";" "\\;" expected_files "${arg_EXPECTED_FILES}")
  set(script_args -DEXE=$<TARGET_FILE:vnmaki> "-DARGS=${arg_ARGS}"
                  -DEXPECTED_EXIT=${arg_EXIT} "-DEXPECTED_STDOUT_FILE=${arg_STDOUT_FILE}"
                  ${data_define} "-DEXPECTED_FILES=${expected_files}")
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
set(vnmaki_default_dir_args run --headless)
vnmaki_isolate_run_data(run_headless_defaults_to_the_current_directory vnmaki_default_dir_args
                        vnmaki_default_dir_data)
add_test(NAME cli.run_headless_defaults_to_the_current_directory
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:vnmaki> "-DARGS=${vnmaki_default_dir_args}"
                 ${vnmaki_default_dir_data} -DEXPECTED_EXIT=0
                 "-DEXPECTED_STDOUT_FILE=${vnmaki_run_fixtures}/demo.transcript" -P
                 "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_stdout_file.cmake"
         WORKING_DIRECTORY "${vnmaki_run_fixtures}/demo")

# --data-dir (headless and in a window) is where saves and the persistent file go. The helper
# above also gives every other run test such a folder; this one checks that the files appear there.
set(vnmaki_saves_data "${CMAKE_CURRENT_BINARY_DIR}/data/run_headless_writes_saves_to_the_data_dir")
vnmaki_add_cli_stdout_file_test(
  run_headless_writes_saves_to_the_data_dir
  ARGS run "${vnmaki_run_fixtures}/saves" --headless
  EXIT 0
  STDOUT_FILE "${vnmaki_run_fixtures}/saves.transcript"
  EXPECTED_FILES "${vnmaki_saves_data}/auto.vnsave" "${vnmaki_saves_data}/quick.vnsave"
                 "${vnmaki_saves_data}/persistent.vnpersist")

# The engine RNG gives the same numbers on every platform and run.
vnmaki_add_cli_stdout_file_test(
  run_headless_rolls_the_same_dice_every_time
  ARGS run "${vnmaki_run_fixtures}/dice" --headless
  EXIT 0
  STDOUT_FILE "${vnmaki_run_fixtures}/dice.transcript")

# Characters, say and narrate (ticket 08). Exit 0 means the story reached its end.
vnmaki_add_cli_stdout_file_test(
  run_headless_plays_characters_to_the_end
  ARGS run "${vnmaki_run_fixtures}/say" --headless
  EXIT 0
  STDOUT_FILE "${vnmaki_run_fixtures}/say.transcript")

vnmaki_add_cli_test(
  run_headless_stops_at_a_menu
  ARGS run "${vnmaki_run_fixtures}/menu" --headless
  EXIT 1
  STDERR_REGEX "vnmaki: the story waits at a menu")

vnmaki_add_cli_test(
  run_headless_reports_script_errors
  ARGS run "${vnmaki_run_fixtures}/broken" --headless
  EXIT 1
  STDERR_REGEX "vnmaki: scripts/start.lua:3: unknown label 'nowhere'")

# Scene, show and hide (ticket 09).
vnmaki_add_cli_stdout_file_test(
  run_headless_prints_scene_show_and_hide
  ARGS run "${vnmaki_run_fixtures}/visuals" --headless
  EXIT 0
  STDOUT_FILE "${vnmaki_run_fixtures}/visuals.transcript")

vnmaki_add_cli_test(
  run_headless_reports_a_missing_image_with_its_script_line
  ARGS run "${vnmaki_run_fixtures}/missing_image" --headless
  EXIT 1
  STDERR_REGEX
    "vnmaki: scripts/start.lua:3: image 'ghost' not found .no images/ghost.png, images/ghost.jpg or images/ghost.webp.")

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

# vnmaki run without --headless (ticket 08): the story in a window, with the say box. The "say"
# and "finished" projects carry fonts/default.ttf; "demo" has no font.
vnmaki_add_cli_test(
  run_window_plays_a_story_with_a_say_box
  ARGS run "${vnmaki_run_fixtures}/say" --frames 3
  EXIT 0
  NO_STDOUT)

# The window closes by itself once the story has ended (the frame limit is far away).
vnmaki_add_cli_test(
  run_window_closes_when_the_story_ends
  ARGS run "${vnmaki_run_fixtures}/finished" --frames 100000
  EXIT 0
  NO_STDOUT)

vnmaki_add_cli_test(
  run_window_reports_a_script_error
  ARGS run "${vnmaki_run_fixtures}/broken_with_font" --frames 100000
  EXIT 1
  NO_STDOUT
  STDERR_REGEX "vnmaki: scripts/start.lua:2: unknown label 'nowhere'")

set(vnmaki_run_window_tests cli.run_window_plays_a_story_with_a_say_box
                            cli.run_window_closes_when_the_story_ends
                            cli.run_window_reports_a_script_error)
set_tests_properties(${vnmaki_run_window_tests} PROPERTIES TIMEOUT 60)
if(vnmaki_gpu_cli_env)
  set_tests_properties(${vnmaki_run_window_tests} PROPERTIES ENVIRONMENT "${vnmaki_gpu_cli_env}")
endif()
if(NOT "$ENV{VNMAKI_REQUIRE_GPU}" STREQUAL "1")
  set_tests_properties(${vnmaki_run_window_tests} PROPERTIES SKIP_REGULAR_EXPRESSION
                                                             "cannot create a GPU device")
endif()

# The font is read before any window opens, so this needs no GPU.
vnmaki_add_cli_test(
  run_window_needs_the_projects_default_font
  ARGS run "${vnmaki_run_fixtures}/demo" --frames 1
  EXIT 1
  NO_STDOUT
  STDERR_REGEX "vnmaki: .*fonts/default.ttf")

vnmaki_add_cli_test(
  run_rejects_frames_with_headless
  ARGS run "${vnmaki_run_fixtures}/demo" --headless --frames 3
  EXIT 2
  NO_STDOUT
  STDERR_REGEX "vnmaki run: --frames needs a window")

vnmaki_add_cli_test(
  run_rejects_a_bad_frame_count
  ARGS run "${vnmaki_run_fixtures}/demo" --frames nope
  EXIT 2
  NO_STDOUT
  STDERR_REGEX "vnmaki run: --frames needs a positive whole number")

vnmaki_add_cli_test(
  run_rejects_a_bad_step_limit
  ARGS run "${vnmaki_run_fixtures}/demo" --headless --max-steps nope
  EXIT 2
  NO_STDOUT
  STDERR_REGEX "vnmaki run: --max-steps needs a positive whole number")

# vnmaki run --profile (ticket 33): one CSV row for each frame, and the loads file. It is in every
# build, and needs a GPU like the other window tests.
set(vnmaki_profile_out "${CMAKE_CURRENT_BINARY_DIR}/profile/run.csv")
add_test(NAME cli.run_profile_writes_a_row_for_each_frame
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:vnmaki>
                 -DPROJECT=${vnmaki_run_fixtures}/say -DFRAMES=5 -DOUT=${vnmaki_profile_out}
                 -DDATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/data/run_profile -P
                 "${CMAKE_CURRENT_SOURCE_DIR}/run_profile.cmake")
set_tests_properties(cli.run_profile_writes_a_row_for_each_frame PROPERTIES TIMEOUT 60)
if(vnmaki_gpu_cli_env)
  set_tests_properties(cli.run_profile_writes_a_row_for_each_frame
                       PROPERTIES ENVIRONMENT "${vnmaki_gpu_cli_env}")
endif()
if(NOT "$ENV{VNMAKI_REQUIRE_GPU}" STREQUAL "1")
  set_tests_properties(cli.run_profile_writes_a_row_for_each_frame
                       PROPERTIES SKIP_REGULAR_EXPRESSION "cannot create a GPU device")
endif()

set(vnmaki_new_precompiled "")
if(NOT VNMAKI_SHADER_COMPILER)
  set(vnmaki_new_precompiled "-DPRECOMPILED_DIR=${VNMAKI_PRECOMPILED_DIR}")
endif()

# The Phase 4 demo (ticket 37): the profile CSV of 120 frames of the template.
add_test(NAME cli.run_profile_of_the_template_has_120_frames
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:vnmaki>
                 -DPROJECT=${PROJECT_SOURCE_DIR}/templates/default -DFRAMES=120
                 ${vnmaki_new_precompiled}
                 -DOUT=${CMAKE_CURRENT_BINARY_DIR}/profile/template.csv
                 -DDATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/data/run_profile_template -P
                 "${CMAKE_CURRENT_SOURCE_DIR}/run_profile.cmake")
set_tests_properties(cli.run_profile_of_the_template_has_120_frames PROPERTIES TIMEOUT 60)
if(vnmaki_gpu_cli_env)
  set_tests_properties(cli.run_profile_of_the_template_has_120_frames
                       PROPERTIES ENVIRONMENT "${vnmaki_gpu_cli_env}")
endif()
if(NOT "$ENV{VNMAKI_REQUIRE_GPU}" STREQUAL "1")
  set_tests_properties(cli.run_profile_of_the_template_has_120_frames
                       PROPERTIES SKIP_REGULAR_EXPRESSION "cannot create a GPU device")
endif()

vnmaki_add_cli_test(
  run_rejects_profile_with_headless
  ARGS run "${vnmaki_run_fixtures}/demo" --headless --profile out.csv
  EXIT 2
  NO_STDOUT
  STDERR_REGEX "vnmaki run: --profile needs a window")

# vnmaki new (ticket 19). The scripts delete the game folder first, so a second run of the test
# starts clean. The second test needs a GPU, like the other window tests.
add_test(NAME cli.new_makes_a_game_and_stops_at_a_second_one
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:vnmaki>
                 -DGAME=${CMAKE_CURRENT_BINARY_DIR}/new/plain -P
                 "${CMAKE_CURRENT_SOURCE_DIR}/run_new.cmake")
add_test(NAME cli.new_game_runs_in_a_window
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:vnmaki> ${vnmaki_new_precompiled}
                 -DGAME=${CMAKE_CURRENT_BINARY_DIR}/new/windowed -DFRAMES=3
                 -DDATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/data/new_game_runs_in_a_window -P
                 "${CMAKE_CURRENT_SOURCE_DIR}/run_new.cmake")
# The presentation demo (ticket 28) in a window: the story starts at its label, so the GPU draws the
# dissolve, the layered sprite with the blink, the camera, the shake and the sepia layer shader.
add_test(NAME cli.new_presentation_runs_in_a_window
         COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:vnmaki> ${vnmaki_new_precompiled}
                 -DGAME=${CMAKE_CURRENT_BINARY_DIR}/new/presentation -DFRAMES=300
                 -DPRESENTATION=ON
                 -DDATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/data/new_presentation_runs_in_a_window -P
                 "${CMAKE_CURRENT_SOURCE_DIR}/run_new.cmake")
set(vnmaki_new_window_tests cli.new_game_runs_in_a_window cli.new_presentation_runs_in_a_window)
set_tests_properties(cli.new_makes_a_game_and_stops_at_a_second_one ${vnmaki_new_window_tests}
                     PROPERTIES TIMEOUT 60)
if(vnmaki_gpu_cli_env)
  set_tests_properties(${vnmaki_new_window_tests} PROPERTIES ENVIRONMENT "${vnmaki_gpu_cli_env}")
endif()
if(NOT "$ENV{VNMAKI_REQUIRE_GPU}" STREQUAL "1")
  set_tests_properties(${vnmaki_new_window_tests} PROPERTIES SKIP_REGULAR_EXPRESSION
                                                             "cannot create a GPU device")
endif()

# Audio (ticket 16), on the projects tools/generators/audio_fixtures.py writes.
set(vnmaki_audio_fixtures "${VNMAKI_FIXTURES_DIR}/audio")
vnmaki_add_cli_stdout_file_test(
  run_headless_prints_audio_commands
  ARGS run "${vnmaki_audio_fixtures}/game" --headless
  EXIT 0
  STDOUT_FILE "${vnmaki_audio_fixtures}/game.transcript")

vnmaki_add_cli_test(
  run_headless_reports_missing_audio_with_its_script_line
  ARGS run "${vnmaki_audio_fixtures}/missing" --headless
  EXIT 1
  STDERR_REGEX
    "vnmaki: scripts/start.lua:3: audio 'bgm/ghost' not found .no audio/bgm/ghost.opus, .ogg, .flac or .wav.")

# vnmaki graph (ticket 34), on the projects tools/generators/graph_fixtures.py writes: the CSV and
# the DOT text, with the lines of the calls and the unreachable labels and dead ends that the
# generator works out for itself.
set(vnmaki_graph_fixtures "${VNMAKI_FIXTURES_DIR}/graph")
vnmaki_add_cli_stdout_file_test(
  graph_prints_the_csv_of_a_project
  ARGS graph "${vnmaki_graph_fixtures}/story" --format csv
  EXIT 0
  STDOUT_FILE "${vnmaki_graph_fixtures}/story.csv")
vnmaki_add_cli_stdout_file_test(
  graph_prints_dot_by_default
  ARGS graph "${vnmaki_graph_fixtures}/story"
  EXIT 0
  STDOUT_FILE "${vnmaki_graph_fixtures}/story.dot")
vnmaki_add_cli_stdout_file_test(
  graph_prints_dot_when_asked
  ARGS graph --format dot "${vnmaki_graph_fixtures}/story"
  EXIT 0
  STDOUT_FILE "${vnmaki_graph_fixtures}/story.dot")
# The template (the Phase 4 demo, ticket 37): the expected CSV comes from graph_fixtures.py.
vnmaki_add_cli_stdout_file_test(
  graph_prints_the_csv_of_the_template
  ARGS graph "${PROJECT_SOURCE_DIR}/templates/default" --format csv
  EXIT 0
  STDOUT_FILE "${vnmaki_graph_fixtures}/template.csv")
vnmaki_add_cli_stdout_file_test(
  graph_prints_the_csv_of_a_random_project
  ARGS graph "${vnmaki_graph_fixtures}/random" --format csv
  EXIT 0
  STDOUT_FILE "${vnmaki_graph_fixtures}/random.csv")

# A folder with no scripts/ folder has no labels.
vnmaki_add_cli_test(
  graph_of_a_folder_without_scripts_has_only_the_header
  ARGS graph "${vnmaki_graph_fixtures}/story/scripts/extra" --format csv
  EXIT 0
  STDOUT_LINE "type,from,to,script,line,flags")

vnmaki_add_cli_test(
  graph_reports_a_missing_folder
  ARGS graph "${vnmaki_graph_fixtures}/does_not_exist"
  EXIT 1
  NO_STDOUT
  STDERR_REGEX "vnmaki: .*does_not_exist.* is not a directory")

vnmaki_add_cli_test(
  graph_needs_a_folder
  ARGS graph
  EXIT 2
  NO_STDOUT
  STDERR_REGEX "^usage: vnmaki graph <dir> .--format dot.csv.")

vnmaki_add_cli_test(
  graph_rejects_an_unknown_format
  ARGS graph "${vnmaki_graph_fixtures}/story" --format xml
  EXIT 2
  NO_STDOUT
  STDERR_REGEX "^usage: vnmaki graph")

vnmaki_add_cli_test(
  graph_rejects_a_second_folder
  ARGS graph "${vnmaki_graph_fixtures}/story" "${vnmaki_graph_fixtures}/random"
  EXIT 2
  NO_STDOUT
  STDERR_REGEX "^usage: vnmaki graph")

# vnmaki graph never opens a window or waits for input, so a hang is a bug.
set_tests_properties(
  cli.graph_prints_the_csv_of_a_project
  cli.graph_prints_dot_by_default
  cli.graph_prints_dot_when_asked
  cli.graph_prints_the_csv_of_a_random_project
  cli.graph_prints_the_csv_of_the_template
  cli.graph_of_a_folder_without_scripts_has_only_the_header
  cli.graph_reports_a_missing_folder
  cli.graph_needs_a_folder
  cli.graph_rejects_an_unknown_format
  cli.graph_rejects_a_second_folder PROPERTIES TIMEOUT 60)

# vnmaki check (ticket 35), on the projects tools/generators/check_fixtures.py writes: one project
# for each finding code, with the CSV that the generator works out for itself. A project with an
# error exits 1; one with only warnings exits 0.
set(vnmaki_check_fixtures "${VNMAKI_FIXTURES_DIR}/check")
set(vnmaki_check_error_cases lua_syntax unknown_label bad_text_tag missing_image missing_audio
                             missing_movie bad_translation_tag po_syntax mixed)
set(vnmaki_check_warning_cases unreachable_label dead_end dynamic_edge non_literal_string
                               project_config project_config_line)
set(vnmaki_check_tests "")
foreach(case IN LISTS vnmaki_check_error_cases vnmaki_check_warning_cases)
  set(exit_code 0)
  if(case IN_LIST vnmaki_check_error_cases)
    set(exit_code 1)
  endif()
  vnmaki_add_cli_stdout_file_test(
    check_${case}
    ARGS check "${vnmaki_check_fixtures}/${case}"
    EXIT ${exit_code}
    STDOUT_FILE "${vnmaki_check_fixtures}/${case}.csv")
  list(APPEND vnmaki_check_tests cli.check_${case})
endforeach()

# The template of `vnmaki new` has no finding: only the header.
vnmaki_add_cli_test(
  check_template_has_no_findings
  ARGS check "${PROJECT_SOURCE_DIR}/templates/default"
  EXIT 0
  STDOUT_LINE "file,line,severity,code,message")

vnmaki_add_cli_test(
  check_reports_a_missing_folder
  ARGS check "${vnmaki_check_fixtures}/does_not_exist"
  EXIT 1
  NO_STDOUT
  STDERR_REGEX "vnmaki: .*does_not_exist.* is not a directory")

vnmaki_add_cli_test(
  check_needs_a_folder
  ARGS check
  EXIT 2
  NO_STDOUT
  STDERR_REGEX "^usage: vnmaki check <dir>")

vnmaki_add_cli_test(
  check_rejects_a_second_folder
  ARGS check "${vnmaki_check_fixtures}/mixed" "${vnmaki_check_fixtures}/lua_syntax"
  EXIT 2
  NO_STDOUT
  STDERR_REGEX "^usage: vnmaki check")

# vnmaki check never opens a window or waits for input, so a hang is a bug.
set_tests_properties(
  ${vnmaki_check_tests}
  cli.check_template_has_no_findings
  cli.check_reports_a_missing_folder
  cli.check_needs_a_folder
  cli.check_rejects_a_second_folder PROPERTIES TIMEOUT 60)

# vnmaki debug (ticket 36). The debugger speaks DAP on stdin and stdout. It needs the dev tools
# and Windows (the non-blocking read of stdin). The headless mode opens no window, so these tests
# need no GPU. Each runs the debugger on a project and a request file that
# tools/generators/debug_fixtures.py writes, with the responses that the generator works out
# itself (the stdin of the process is the request file, which ends: EOF, not a hang).
set(vnmaki_debug_fixtures "${VNMAKI_FIXTURES_DIR}/debug")
if(VNMAKI_DEV_TOOLS AND WIN32)
  foreach(session session finish)
    set(data_dir "${CMAKE_CURRENT_BINARY_DIR}/data/debug_${session}")
    add_test(
      NAME cli.debug_${session}
      COMMAND
        ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:vnmaki>
        "-DARGS=debug;${vnmaki_debug_fixtures}/project;--headless;--data-dir;${data_dir}"
        -DEXPECTED_EXIT=0 -DDATA_DIR=${data_dir}
        -DINPUT_FILE=${vnmaki_debug_fixtures}/${session}.in.dap
        -DEXPECTED_STDOUT_FILE=${vnmaki_debug_fixtures}/${session}.out.dap -P
        "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_dap.cmake")
    set_tests_properties(cli.debug_${session} PROPERTIES TIMEOUT 120)
  endforeach()
endif()

# The Phase 4 demo (ticket 37): the same on the template, in templates/default: a breakpoint in
# `start`, a step, `store.affection` and a goto to `warm`.
if(VNMAKI_DEV_TOOLS AND WIN32)
  set(data_dir "${CMAKE_CURRENT_BINARY_DIR}/data/debug_template")
  add_test(
    NAME cli.debug_template
    COMMAND
      ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:vnmaki>
      "-DARGS=debug;${PROJECT_SOURCE_DIR}/templates/default;--headless;--data-dir;${data_dir}"
      -DEXPECTED_EXIT=0 -DDATA_DIR=${data_dir}
      -DINPUT_FILE=${vnmaki_debug_fixtures}/template.in.dap
      -DEXPECTED_STDOUT_FILE=${vnmaki_debug_fixtures}/template.out.dap -P
      "${CMAKE_CURRENT_SOURCE_DIR}/run_cli_dap.cmake")
  set_tests_properties(cli.debug_template PROPERTIES TIMEOUT 120)
endif()

vnmaki_add_cli_test(
  debug_rejects_an_unknown_option
  ARGS debug --frobnicate
  EXIT 2
  NO_STDOUT
  STDERR_REGEX "vnmaki debug: unexpected argument '--frobnicate'.*usage: vnmaki debug")
if(NOT VNMAKI_DEV_TOOLS)
  vnmaki_add_cli_test(
    debug_needs_the_dev_tools
    ARGS debug "${PROJECT_SOURCE_DIR}/templates/default"
    EXIT 1
    NO_STDOUT
    STDERR_REGEX "vnmaki debug: this build has no debugger")
endif()
set_tests_properties(cli.debug_rejects_an_unknown_option PROPERTIES TIMEOUT 60)

# vnmaki extract-strings (ticket 40), on the projects tools/generators/l10n_fixtures.py writes. The
# run works on a copy in the build tree, then a second run and `vnmaki check` must accept it.
set(vnmaki_l10n_fixtures "${VNMAKI_FIXTURES_DIR}/l10n")
add_test(
  NAME cli.extract_strings_writes_the_template_and_updates_the_po_files
  COMMAND
    ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:vnmaki> "-DPROJECT=${vnmaki_l10n_fixtures}/extract"
    "-DEXPECTED=${vnmaki_l10n_fixtures}/extract.expected"
    "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/work/extract" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/run_extract.cmake")

vnmaki_add_cli_test(
  extract_strings_stops_at_a_po_file_it_cannot_read
  ARGS extract-strings "${vnmaki_l10n_fixtures}/broken"
  EXIT 1
  NO_STDOUT
  STDERR_REGEX "vnmaki: translations/ja.po:5: string is not closed")

vnmaki_add_cli_test(
  extract_strings_reports_a_missing_folder
  ARGS extract-strings "${vnmaki_l10n_fixtures}/does_not_exist"
  EXIT 1
  NO_STDOUT
  STDERR_REGEX "is not a directory")

vnmaki_add_cli_test(
  extract_strings_needs_a_folder
  ARGS extract-strings
  EXIT 2
  NO_STDOUT
  STDERR_REGEX "^usage: vnmaki extract-strings <dir>")

vnmaki_add_cli_test(
  extract_strings_rejects_a_second_folder
  ARGS extract-strings "${vnmaki_l10n_fixtures}/extract" "${vnmaki_l10n_fixtures}/broken"
  EXIT 2
  NO_STDOUT
  STDERR_REGEX "^usage: vnmaki extract-strings")

# vnmaki extract-strings never opens a window or waits for input, so a hang is a bug.
set_tests_properties(
  cli.extract_strings_writes_the_template_and_updates_the_po_files
  cli.extract_strings_stops_at_a_po_file_it_cannot_read
  cli.extract_strings_reports_a_missing_folder
  cli.extract_strings_needs_a_folder
  cli.extract_strings_rejects_a_second_folder PROPERTIES TIMEOUT 60)

# vnmaki import (ticket 44), on the projects tools/generators/import_fixtures.py writes. The shader
# is only imported by a build that has the shader compiler (VNMAKI_SHADER_COMPILER).
set(vnmaki_import_fixtures "${VNMAKI_FIXTURES_DIR}/import")
if(VNMAKI_SHADER_COMPILER)
  set(vnmaki_import_shaders ON)
else()
  set(vnmaki_import_shaders OFF)
endif()
add_test(
  NAME cli.import_writes_the_outputs_and_uses_the_cache
  COMMAND
    ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:vnmaki> "-DPROJECT=${vnmaki_import_fixtures}/project"
    "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/work/import" -DSHADERS=${vnmaki_import_shaders} -P
    "${CMAKE_CURRENT_SOURCE_DIR}/run_import.cmake")

vnmaki_add_cli_test(
  import_reports_a_missing_buffer_and_texture
  ARGS import "${vnmaki_import_fixtures}/project_broken"
  EXIT 1
  NO_STDOUT
  STDERR_REGEX "models/missing.gltf: buffer 0 'nothing.bin' not found.*texture image 0 'nothing.png' not found")

vnmaki_add_cli_test(
  import_reports_a_missing_folder
  ARGS import "${vnmaki_import_fixtures}/does_not_exist"
  EXIT 1
  NO_STDOUT
  STDERR_REGEX "does_not_exist.* is not a directory")

vnmaki_add_cli_test(
  import_needs_a_folder
  ARGS import
  EXIT 2
  NO_STDOUT
  STDERR_REGEX "^usage: vnmaki import <dir>")

vnmaki_add_cli_test(
  import_rejects_a_second_folder
  ARGS import "${vnmaki_import_fixtures}/project" "${vnmaki_import_fixtures}/project_broken"
  EXIT 2
  NO_STDOUT
  STDERR_REGEX "^usage: vnmaki import")

# vnmaki import never opens a window or waits for input, so a hang is a bug.
set_tests_properties(
  cli.import_writes_the_outputs_and_uses_the_cache
  cli.import_reports_a_missing_buffer_and_texture
  cli.import_reports_a_missing_folder
  cli.import_needs_a_folder
  cli.import_rejects_a_second_folder PROPERTIES TIMEOUT 120)
