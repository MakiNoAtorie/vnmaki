# Packs a game that has a Lua plugin with `vnmaki build windows` and plays it from the packs only.
#
# Usage: cmake -DEXE=<path> -DGAME=<project folder> -DWORK=<folder> -P run_plugin_packs.cmake
#
# The plugin files are Vfs files, so the packs hold them like any other file. The game is copied
# first and the copy is deleted before the packs run, so only the packs can supply the plugin.

function(run name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "COMMAND")
  execute_process(
    COMMAND ${arg_COMMAND}
    RESULT_VARIABLE exit
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    TIMEOUT 120)
  if(NOT exit EQUAL 0)
    message(FATAL_ERROR "${name}: exit ${exit}, stdout [${out}] stderr [${err}]")
  endif()
  string(REPLACE "\r\n" "\n" out "${out}")
  set(${name}_out "${out}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
file(COPY "${GAME}/" DESTINATION "${WORK}/game")

run(loose COMMAND "${EXE}" run "${WORK}/game" --headless --data-dir "${WORK}/data_loose")
if(NOT loose_out MATCHES "narrate HI!")
  message(FATAL_ERROR "the loose game did not use the plugin: [${loose_out}]")
endif()

run(build COMMAND "${EXE}" build windows "${WORK}/game" --out "${WORK}/packed" --runner "${EXE}")
if(NOT EXISTS "${WORK}/packed/game.vnpak")
  message(FATAL_ERROR "build did not write game.vnpak: [${build_out}]")
endif()
file(REMOVE_RECURSE "${WORK}/game")

run(packs COMMAND "${EXE}" run "${WORK}/packed" --headless --data-dir "${WORK}/data_packs")
if(NOT packs_out STREQUAL loose_out)
  message(FATAL_ERROR "the packs play differently:\n[${packs_out}]\nagainst\n[${loose_out}]")
endif()
