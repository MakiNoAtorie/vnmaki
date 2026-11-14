# Runs `vnmaki import --out` twice on a copy of a project and checks the output.
#
# Usage: cmake -DEXE=<path> -DPROJECT=<folder> -DWORK=<folder> -DSHADERS=<ON|OFF> -P run_import.cmake
#
# PROJECT is copied to WORK (deleted first). SHADERS is ON when this build has a shader compiler;
# if not, the shaders of the copy are removed, because they cannot be compiled.
#
# The first run must import every file and write the outputs, the second must take them from the
# cache and write the same bytes. WORK/.vnmaki/cache has the entries.

function(run_import out_var)
  execute_process(
    COMMAND "${EXE}" import "${WORK}" --out "${WORK}_out"
    RESULT_VARIABLE exit
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    TIMEOUT 120)
  if(NOT exit EQUAL 0)
    message(FATAL_ERROR "import: exit ${exit}, stdout [${out}] stderr [${err}]")
  endif()
  string(REPLACE "\r\n" "\n" out "${out}")
  set(${out_var} "${out}" PARENT_SCOPE)
endfunction()

function(expect_line text line)
  string(FIND "${text}" "${line}\n" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "expected the line [${line}] in [${text}]")
  endif()
endfunction()

file(REMOVE_RECURSE "${WORK}" "${WORK}_out")
file(COPY "${PROJECT}/" DESTINATION "${WORK}")
if(NOT SHADERS)
  file(REMOVE_RECURSE "${WORK}/shaders")
endif()

set(outputs audio/tone.opus fonts/default.en.ttf fonts/default.ja.ttf)
set(sources audio/tone.wav fonts/default.ttf fonts/default.ttf)
if(SHADERS)
  list(APPEND outputs shaders/tint.frag.vnshader)
  list(APPEND sources shaders/tint.frag.hlsl)
endif()

run_import(first)
expect_line("${first}" "models/triangle.gltf (valid)")
foreach(source output IN ZIP_LISTS sources outputs)
  expect_line("${first}" "${source} -> ${output} (imported)")
  if(NOT EXISTS "${WORK}_out/${output}")
    message(FATAL_ERROR "import did not write ${output}")
  endif()
  file(SIZE "${WORK}_out/${output}" size)
  if(size EQUAL 0)
    message(FATAL_ERROR "${output} is empty")
  endif()
  file(SHA256 "${WORK}_out/${output}" hash_${output})
endforeach()
file(GLOB cache "${WORK}/.vnmaki/cache/*")
list(LENGTH cache cache_count)
list(LENGTH outputs output_count)
if(NOT cache_count EQUAL output_count)
  message(FATAL_ERROR "expected ${output_count} cache entries, got [${cache}]")
endif()

# The source files stay as they are, and the import adds nothing but the cache and the output.
if(NOT EXISTS "${WORK}/audio/tone.wav" OR EXISTS "${WORK}/audio/tone.opus")
  message(FATAL_ERROR "import changed the project files")
endif()

file(REMOVE_RECURSE "${WORK}_out")
run_import(second)
foreach(source output IN ZIP_LISTS sources outputs)
  expect_line("${second}" "${source} -> ${output} (cached)")
  file(SHA256 "${WORK}_out/${output}" again)
  if(NOT again STREQUAL hash_${output})
    message(FATAL_ERROR "${output} changed on the second run")
  endif()
endforeach()
