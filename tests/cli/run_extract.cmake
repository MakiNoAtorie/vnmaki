# Runs `vnmaki extract-strings` on a copy of a project and compares the files it writes.
#
# Usage: cmake -DEXE=<path> -DPROJECT=<folder> -DEXPECTED=<folder> -DWORK=<folder>
#              -P run_extract.cmake
#
# PROJECT is copied to WORK (deleted first). EXPECTED has the expected `translations/*` files and
# `stdout.txt`. A second run must change nothing, and `vnmaki check` must accept the result.

function(read_text path out_var)
  file(READ "${path}" text)
  string(REPLACE "\r\n" "\n" text "${text}")
  set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK}")
file(COPY "${PROJECT}/" DESTINATION "${WORK}")

execute_process(
  COMMAND "${EXE}" extract-strings "${WORK}"
  RESULT_VARIABLE exit
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)
if(NOT exit EQUAL 0)
  message(FATAL_ERROR "extract-strings: exit ${exit}, stdout [${out}] stderr [${err}]")
endif()
string(REPLACE "\r\n" "\n" out "${out}")
read_text("${EXPECTED}/stdout.txt" expected_out)
if(NOT out STREQUAL expected_out)
  message(FATAL_ERROR "stdout: expected [${expected_out}], got [${out}]")
endif()

file(GLOB_RECURSE expected_files RELATIVE "${EXPECTED}" "${EXPECTED}/translations/*")
list(LENGTH expected_files expected_count)
if(NOT expected_count EQUAL 3)
  message(FATAL_ERROR "expected 3 files, the fixture has [${expected_files}]")
endif()
foreach(file IN LISTS expected_files)
  if(NOT EXISTS "${WORK}/${file}")
    message(FATAL_ERROR "extract-strings did not write ${file}")
  endif()
  read_text("${EXPECTED}/${file}" expected)
  read_text("${WORK}/${file}" actual)
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "${file}: expected [${expected}], got [${actual}]")
  endif()
endforeach()
file(GLOB leftovers "${WORK}/translations/*.tmp")
if(leftovers)
  message(FATAL_ERROR "extract-strings left ${leftovers}")
endif()

execute_process(
  COMMAND "${EXE}" extract-strings "${WORK}"
  RESULT_VARIABLE exit
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)
if(NOT exit EQUAL 0)
  message(FATAL_ERROR "second extract-strings: exit ${exit}, stderr [${err}]")
endif()
foreach(file IN LISTS expected_files)
  read_text("${EXPECTED}/${file}" expected)
  read_text("${WORK}/${file}" actual)
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "second run changed ${file}: [${actual}]")
  endif()
endforeach()

execute_process(
  COMMAND "${EXE}" check "${WORK}"
  RESULT_VARIABLE exit
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)
if(NOT exit EQUAL 0)
  message(FATAL_ERROR "check after extract-strings: exit ${exit}, stdout [${out}]")
endif()
