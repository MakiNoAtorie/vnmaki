# Runs the vnmaki CLI once and checks its exit code and exact stdout.
#
# Usage: cmake -DEXE=<path> -DARGS=<;-list> -DEXPECTED_EXIT=<int>
#              [-DEXPECTED_STDOUT_LINE=<text> | -DEXPECTED_NO_STDOUT=ON]
#              [-DEXPECTED_STDERR_REGEX=<regex>] -P run_cli.cmake
#
# EXPECTED_STDOUT_LINE is the whole of stdout: exactly one line, newline
# terminated. Line endings are normalised to "\n" so this holds on Windows.

execute_process(
  COMMAND "${EXE}" ${ARGS}
  RESULT_VARIABLE actual_exit
  OUTPUT_VARIABLE actual_stdout
  ERROR_VARIABLE actual_stderr)

string(REPLACE "\r\n" "\n" actual_stdout "${actual_stdout}")
string(REPLACE "\r\n" "\n" actual_stderr "${actual_stderr}")

set(failed FALSE)
if(NOT actual_exit STREQUAL EXPECTED_EXIT)
  message(SEND_ERROR "exit code: expected ${EXPECTED_EXIT}, got ${actual_exit}")
  set(failed TRUE)
endif()
if(DEFINED EXPECTED_STDOUT_LINE AND NOT actual_stdout STREQUAL "${EXPECTED_STDOUT_LINE}\n")
  message(SEND_ERROR "stdout: expected [${EXPECTED_STDOUT_LINE}\n], got [${actual_stdout}]")
  set(failed TRUE)
endif()
if(EXPECTED_NO_STDOUT AND NOT actual_stdout STREQUAL "")
  message(SEND_ERROR "stdout: expected nothing, got [${actual_stdout}]")
  set(failed TRUE)
endif()
if(DEFINED EXPECTED_STDERR_REGEX AND NOT actual_stderr MATCHES "${EXPECTED_STDERR_REGEX}")
  message(SEND_ERROR "stderr: expected to match [${EXPECTED_STDERR_REGEX}], got [${actual_stderr}]")
  set(failed TRUE)
endif()
if(failed)
  message(FATAL_ERROR "vnmaki ${ARGS}: stdout [${actual_stdout}] stderr [${actual_stderr}]")
endif()
