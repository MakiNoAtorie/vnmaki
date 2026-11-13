# Builds the template with `vnmaki build windows` and plays the result from the packs only.
#
# Usage: cmake -DEXE=<path> -DWORK=<folder> [-DPRECOMPILED_DIR=<folder>] -P run_build.cmake
#
# The game is made with `vnmaki new`; its title screen is replaced by a jump to the label
# "presentation", because the headless runner cannot answer a screen. The runner is EXE itself (a
# build has no release sibling in the test tree). The loose game is deleted before the packs run,
# so they must hold everything. PRECOMPILED_DIR is given for a build that has no shader compiler:
# it cannot import a shader, so the game gets the `.vnshader` file in place of the `.hlsl` file.

function(run name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "EXPECT_EXIT;WORKING_DIRECTORY" "COMMAND")
  if(NOT arg_EXPECT_EXIT)
    set(arg_EXPECT_EXIT 0)
  endif()
  if(NOT arg_WORKING_DIRECTORY)
    set(arg_WORKING_DIRECTORY "${WORK}")
  endif()
  execute_process(
    COMMAND ${arg_COMMAND}
    WORKING_DIRECTORY "${arg_WORKING_DIRECTORY}"
    RESULT_VARIABLE exit
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    TIMEOUT 120)
  if(NOT exit EQUAL arg_EXPECT_EXIT)
    message(FATAL_ERROR "${name}: exit ${exit}, stdout [${out}] stderr [${err}]")
  endif()
  string(REPLACE "\r\n" "\n" out "${out}")
  set(${name}_out "${out}" PARENT_SCOPE)
  set(${name}_err "${err}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(GAME "${WORK}/game")
set(PACKED "${WORK}/packed")

run(new COMMAND "${EXE}" new "${GAME}")
file(READ "${GAME}/scripts/main.lua" main)
string(REGEX REPLACE "call_screen\\(\"title\"[^\n]*" "jump(\"presentation\")" patched "${main}")
if(patched STREQUAL main)
  message(FATAL_ERROR "scripts/main.lua has no title screen to replace")
endif()
file(WRITE "${GAME}/scripts/main.lua" "${patched}")
if(DEFINED PRECOMPILED_DIR)
  file(COPY "${PRECOMPILED_DIR}/shaders/sepia.frag.vnshader" DESTINATION "${GAME}/shaders")
  file(REMOVE "${GAME}/shaders/sepia.frag.hlsl")
endif()

run(loose COMMAND "${EXE}" run "${GAME}" --headless --data-dir "${WORK}/data_loose")
if(NOT loose_out MATCHES "narrate The end")
  message(FATAL_ERROR "the loose game did not play to its end: [${loose_out}]")
endif()

run(build COMMAND "${EXE}" build windows "${GAME}" --out "${PACKED}" --runner "${EXE}")
if(NOT build_out MATCHES "packed [0-9]+ files into")
  message(FATAL_ERROR "build printed [${build_out}]")
endif()
foreach(file game.vnpak game.exe SDL3.dll)
  if(NOT EXISTS "${PACKED}/${file}")
    message(FATAL_ERROR "build did not write ${file}")
  endif()
endforeach()
file(SIZE "${PACKED}/game.exe" runner_size)
file(SIZE "${EXE}" exe_size)
if(NOT runner_size EQUAL exe_size)
  message(FATAL_ERROR "game.exe is not a copy of the runner")
endif()

# The packs must hold everything, the imported files too.
file(REMOVE_RECURSE "${GAME}")

run(packs COMMAND "${EXE}" run "${PACKED}" --headless --data-dir "${WORK}/data_packs")
if(NOT packs_out STREQUAL loose_out)
  message(FATAL_ERROR "the packs play differently:\n[${packs_out}]\nagainst\n[${loose_out}]")
endif()

# The runner next to game.vnpak plays it with no folder argument.
run(runner COMMAND "${PACKED}/game.exe" --headless --data-dir "${WORK}/data_runner"
    WORKING_DIRECTORY "${PACKED}")
if(NOT runner_out STREQUAL loose_out)
  message(FATAL_ERROR "game.exe plays differently:\n[${runner_out}]\nagainst\n[${loose_out}]")
endif()
run(window COMMAND "${PACKED}/game.exe" --frames 3 --data-dir "${WORK}/data_window"
    WORKING_DIRECTORY "${PACKED}")

# A bad pak is an error message, not a crash.
file(MAKE_DIRECTORY "${WORK}/bad")
file(WRITE "${WORK}/bad/game.vnpak" "VNPK this is not a pak at all, but it has a magic number")
run(bad EXPECT_EXIT 1 COMMAND "${EXE}" run "${WORK}/bad" --headless --data-dir "${WORK}/data_bad")
if(NOT bad_err MATCHES "vnmaki: .*game.vnpak")
  message(FATAL_ERROR "a bad pak gave [${bad_err}]")
endif()
