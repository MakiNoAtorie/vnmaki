# Black-box tests of the vnmaki executable: each runs it as a process.
function(vnmaki_add_cli_test name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "NO_STDOUT" "EXIT;STDOUT_LINE;STDERR_REGEX"
                        "ARGS")
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
