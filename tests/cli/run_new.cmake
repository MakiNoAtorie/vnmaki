# Makes a game with `vnmaki new`, checks that a second `new` stops, and optionally plays it.
#
# Usage: cmake -DEXE=<path> -DGAME=<folder> [-DFRAMES=<n> -DDATA_DIR=<path> [-DPRESENTATION=ON]]
#        -P run_new.cmake
#
# GAME and DATA_DIR are deleted first. With FRAMES the game runs in a window for that many frames,
# with its saves in DATA_DIR (--data-dir). With PRESENTATION the story starts at the label
# "presentation" in auto mode (the title screen and the menu need a click, and so does each line).

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
foreach(file scripts/main.lua scripts/presentation.lua fonts/default.ttf images/bg/classroom.png
             audio/bgm/theme.wav shaders/sepia.frag.hlsl movies/clip.webm ASSETS.txt)
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
  if(PRESENTATION)
    file(READ "${GAME}/scripts/main.lua" main)
    string(REGEX REPLACE "call_screen\\(\"title\"[^\n]*" "auto(true) jump(\"presentation\")" patched
                         "${main}")
    if(patched STREQUAL main)
      message(FATAL_ERROR "scripts/main.lua has no title screen to replace")
    endif()
    file(WRITE "${GAME}/scripts/main.lua" "${patched}")
  endif()
  execute_process(
    COMMAND "${EXE}" run "${GAME}" --frames ${FRAMES} --data-dir "${DATA_DIR}"
    RESULT_VARIABLE exit
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
  if(NOT exit EQUAL 0)
    message(FATAL_ERROR "vnmaki run: exit ${exit}, stdout [${out}] stderr [${err}]")
  endif()
endif()
