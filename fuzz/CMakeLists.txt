# libFuzzer targets for loaders of untrusted data (docs/SPEC.md section 5).
# Enabled with VNMAKI_BUILD_FUZZERS=ON under LLVM Clang (the `fuzz` preset).
#
# vnmaki_add_fuzzer(<name> <source>...) builds fuzz_<name> and registers a
# short ctest smoke run so every fuzz target is exercised in CI.
function(vnmaki_add_fuzzer name)
  set(target fuzz_${name})
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE vnmaki::engine vnmaki::options)
  target_link_options(${target} PRIVATE -fsanitize=fuzzer)
  add_test(NAME fuzz.${name}.smoke COMMAND ${target} -runs=10000 -seed=1)
endfunction()

vnmaki_add_fuzzer(noop noop_fuzzer.cpp)
vnmaki_add_fuzzer(vfs_path vfs_path_fuzzer.cpp)

# Image decoder (ticket 05). Random bytes rarely get past a PNG or JPEG header, so a second
# smoke run starts from the decoder test fixtures (seeds, never written to) and keeps what it
# finds in a corpus directory in the build tree.
vnmaki_add_fuzzer(image image_fuzzer.cpp)
if(TARGET vnmaki_fixtures_render)
  add_dependencies(fuzz_image vnmaki_fixtures_render)
  set(vnmaki_image_corpus "${CMAKE_CURRENT_BINARY_DIR}/corpus/image")
  file(MAKE_DIRECTORY "${vnmaki_image_corpus}")
  add_test(NAME fuzz.image.seeded COMMAND fuzz_image -runs=10000 -seed=1 "${vnmaki_image_corpus}"
                                          "${PROJECT_BINARY_DIR}/fixtures/render/images")
endif()

# Script runtime (ticket 06).
vnmaki_add_fuzzer(script_loader script_loader_fuzzer.cpp)
vnmaki_add_fuzzer(project_config project_config_fuzzer.cpp)

# Scene snapshots (ticket 09).
vnmaki_add_fuzzer(scene_state scene_state_fuzzer.cpp)

# Font loading (ticket 07). As for images, random bytes rarely make a font, so a second smoke
# run starts from the font fixtures.
vnmaki_add_fuzzer(font font_fuzzer.cpp)
if(TARGET vnmaki_fixtures_text)
  add_dependencies(fuzz_font vnmaki_fixtures_text)
  set(vnmaki_font_corpus "${CMAKE_CURRENT_BINARY_DIR}/corpus/font")
  file(MAKE_DIRECTORY "${vnmaki_font_corpus}")
  add_test(NAME fuzz.font.seeded COMMAND fuzz_font -runs=10000 -seed=1 "${vnmaki_font_corpus}"
                                         "${PROJECT_BINARY_DIR}/fixtures/text/fonts")
endif()

# Text tags (ticket 17). A tag needs a "{" and a known name, so a dictionary gives libFuzzer the
# pieces.
vnmaki_add_fuzzer(markup markup_fuzzer.cpp markup_properties.hpp)
add_test(NAME fuzz.markup.dictionary
         COMMAND fuzz_markup -runs=10000 -seed=1 "-dict=${CMAKE_CURRENT_SOURCE_DIR}/markup.dict")
