# Makes a game with `vnmaki new`, checks that a second `new` stops, and optionally plays it.
#
# Usage: cmake -DEXE=<path> -DGAME=<folder> [-DFRAMES=<n> -DDATA_DIR=<path>] -P run_new.cmake
#
# GAME and DATA_DIR are deleted first. With FRAMES the game runs in a window for that many frames,
# with its saves in DATA_DIR (--data-dir).

file(REMOVE_RECURSE "${GAME}")
execute_process(
  COMMAND "${EXE}" new "${GAME}"
  RESULT_VARIABLE exit
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)
if(NOT exit EQUAL 0)
  message(FATAL_ERROR "vnmaki new: exit ${exit}, stdout [${out}] stderr [${err}]")
endif()

get_filename_component(name "${GAME}" NAME)
file(READ "${GAME}/project.toml" project)
if(NOT project MATCHES "title = \"${name}\"")
  message(FATAL_ERROR "project.toml has no title '${name}': [${project}]")
endif()
foreach(file scripts/main.lua fonts/default.ttf images/bg/classroom.png audio/bgm/theme.wav
             ASSETS.txt)
  if(NOT EXISTS "${GAME}/${file}")
    message(FATAL_ERROR "vnmaki new did not write ${GAME}/${file}")
  endif()
endforeach()

execute_process(
  COMMAND "${EXE}" new "${GAME}"
  RESULT_VARIABLE exit
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)
if(NOT exit EQUAL 1 OR NOT err MATCHES "is not an empty folder")
  message(FATAL_ERROR "a second vnmaki new: exit ${exit}, stdout [${out}] stderr [${err}]")
endif()

if(DEFINED FRAMES)
  file(REMOVE_RECURSE "${DATA_DIR}")
  execute_process(
    COMMAND "${EXE}" run "${GAME}" --frames ${FRAMES} --data-dir "${DATA_DIR}"
    RESULT_VARIABLE exit
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
  if(NOT exit EQUAL 0)
    message(FATAL_ERROR "vnmaki run: exit ${exit}, stdout [${out}] stderr [${err}]")
  endif()
endif()
