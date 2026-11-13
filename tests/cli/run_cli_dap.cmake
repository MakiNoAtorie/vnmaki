# Runs `vnmaki debug` once with a file as its stdin (a DAP client's requests) and checks the exit
# code and that stdout equals the expected responses and events.
#
# Usage: cmake -DEXE=<path> -DARGS=<;-list> -DEXPECTED_EXIT=<int> -DINPUT_FILE=<path>
#              -DEXPECTED_STDOUT_FILE=<path> [-DDATA_DIR=<path>] -P run_cli_dap.cmake
#
# The input ends after its last request (EOF), so a debugger that waits for more input gets EOF
# and not a hang. The TIMEOUT stops a debugger that never ends: it fails the test.
# DATA_DIR is deleted first (it is the --data-dir of the run). Line endings are normalised to "\n"
# on both sides, so the compare holds whatever the platform.

if(DEFINED DATA_DIR)
  file(REMOVE_RECURSE "${DATA_DIR}")
endif()

execute_process(
  COMMAND "${EXE}" ${ARGS}
  INPUT_FILE "${INPUT_FILE}"
  TIMEOUT 60
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
  # Show the messages one by one, so the first difference is easy to find.
  string(REPLACE "Content-Length" "\nContent-Length" expected_lines "${expected_stdout}")
  string(REPLACE "Content-Length" "\nContent-Length" actual_lines "${actual_stdout}")
  message(SEND_ERROR "stdout differs.\nexpected:${expected_lines}\n\nactual:${actual_lines}")
  set(failed TRUE)
endif()
if(failed)
  message(FATAL_ERROR "vnmaki ${ARGS}: stderr [${actual_stderr}]")
endif()
