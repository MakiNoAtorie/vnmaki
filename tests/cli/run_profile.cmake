# Runs `vnmaki run <PROJECT> --frames <FRAMES> --profile <OUT>` and checks the two CSV files: the
# header and one row for each frame in OUT, and the header and the font load in the _loads file.
#
# Usage: cmake -DEXE=<path> -DPROJECT=<dir> -DFRAMES=<n> -DOUT=<file.csv> -DDATA_DIR=<path>
#        -P run_profile.cmake

file(REMOVE_RECURSE "${DATA_DIR}")
get_filename_component(out_dir "${OUT}" DIRECTORY)
get_filename_component(out_name "${OUT}" NAME_WE)
set(loads "${out_dir}/${out_name}_loads.csv")
file(REMOVE "${OUT}" "${loads}")
file(MAKE_DIRECTORY "${out_dir}")

execute_process(
  COMMAND "${EXE}" run "${PROJECT}" --frames ${FRAMES} --profile "${OUT}" --data-dir "${DATA_DIR}"
  RESULT_VARIABLE exit
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)
if(NOT exit EQUAL 0)
  message(FATAL_ERROR "vnmaki run --profile: exit ${exit}, stdout [${out}] stderr [${err}]")
endif()

foreach(file "${OUT}" "${loads}")
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "the run did not write ${file}")
  endif()
endforeach()

file(STRINGS "${OUT}" rows)
list(LENGTH rows row_count)
math(EXPR expected_rows "${FRAMES} + 1")
list(GET rows 0 header)
set(expected_header
    "frame,events_us,update_us,prepare_us,draw_us,total_us,updates,passes,quads,draw_calls,texture_bytes,target_bytes,textures,targets"
)
if(NOT header STREQUAL expected_header)
  message(FATAL_ERROR "frames header: expected [${expected_header}], got [${header}]")
endif()
if(NOT row_count EQUAL expected_rows)
  message(FATAL_ERROR "frames file: expected ${expected_rows} lines, got ${row_count}")
endif()
# Every row has 14 numbers.
list(REMOVE_AT rows 0)
foreach(row IN LISTS rows)
  string(REPLACE "," ";" fields "${row}")
  list(LENGTH fields field_count)
  if(NOT row MATCHES "^[0-9,]+$" OR NOT field_count EQUAL 14)
    message(FATAL_ERROR "bad frame row [${row}]")
  endif()
endforeach()

file(STRINGS "${loads}" load_rows)
list(GET load_rows 0 load_header)
if(NOT load_header STREQUAL "path,time_us")
  message(FATAL_ERROR "loads header: got [${load_header}]")
endif()
list(JOIN load_rows "\n" load_text)
if(NOT load_text MATCHES "\nfonts/default.ttf,[0-9]+")
  message(FATAL_ERROR "the loads file has no row for fonts/default.ttf: [${load_text}]")
endif()
