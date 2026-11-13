# Runs the vnmaki CLI once and checks its exit code, that stdout equals the
# contents of a file, and optionally stderr against a regex.
#
# Usage: cmake -DEXE=<path> -DARGS=<;-list> -DEXPECTED_EXIT=<int>
#              -DEXPECTED_STDOUT_FILE=<path> [-DEXPECTED_STDERR_REGEX=<regex>]
#              [-DDATA_DIR=<path>] [-DEXPECTED_FILES=<;-list>] -P run_cli_stdout_file.cmake
#
# DATA_DIR is deleted first (it is the --data-dir of the run). EXPECTED_FILES must exist
# afterwards.
#
# Line endings are normalised to "\n" on both sides so this holds on Windows.

if(DEFINED DATA_DIR)
  file(REMOVE_RECURSE "${DATA_DIR}")
endif()

execute_process(
  COMMAND "${EXE}" ${ARGS}
  RESULT_VARIABLE actual_exit
  OUTPUT_VARIABLE actual_stdout
  ERROR_VARIABLE actual_stderr)

file(READ "${EXPECTED_STDOUT_FILE}" expected_stdout)
string(REPLACE "\r\n" "\n" expected_stdout "${expected_stdout}")
string(REPLACE "\r\n" "\n" actual_stdout "${actual_stdout}")
string(REPLACE "\r\n" "\n" actual_stderr "${actual_stderr}")

set(failed FALSE)
if(NOT actual_exit STREQUAL EXPECTED_EXIT)
  message(SEND_ERROR "exit code: expected ${EXPECTED_EXIT}, got ${actual_exit}")
  set(failed TRUE)
endif()
if(NOT actual_stdout STREQUAL expected_stdout)
  message(SEND_ERROR "stdout: expected [${expected_stdout}], got [${actual_stdout}]")
  set(failed TRUE)
endif()
if(DEFINED EXPECTED_STDERR_REGEX AND NOT actual_stderr MATCHES "${EXPECTED_STDERR_REGEX}")
  message(SEND_ERROR "stderr: expected to match [${EXPECTED_STDERR_REGEX}], got [${actual_stderr}]")
  set(failed TRUE)
endif()
foreach(file IN LISTS EXPECTED_FILES)
  if(NOT EXISTS "${file}")
    message(SEND_ERROR "expected the run to write ${file}")
    set(failed TRUE)
  endif()
endforeach()
if(failed)
  message(FATAL_ERROR "vnmaki ${ARGS}: stdout [${actual_stdout}] stderr [${actual_stderr}]")
endif()
