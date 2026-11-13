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
vnmaki_add_fuzzer(image image_fuzzer.cpp image_properties.hpp)
if(TARGET vnmaki_fixtures_render)
  add_dependencies(fuzz_image vnmaki_fixtures_render vnmaki_fixtures_webp)
  set(vnmaki_image_corpus "${CMAKE_CURRENT_BINARY_DIR}/corpus/image")
  file(MAKE_DIRECTORY "${vnmaki_image_corpus}")
  add_test(NAME fuzz.image.seeded
           COMMAND fuzz_image -runs=10000 -seed=1 "${vnmaki_image_corpus}"
                   "${PROJECT_BINARY_DIR}/fixtures/render/images"
                   "${PROJECT_BINARY_DIR}/fixtures/webp/images")
endif()

# WebP (ticket 31). The input is the data of the VP8L chunk (the target adds the RIFF container),
# so the second smoke run starts from the VP8L fixture.
vnmaki_add_fuzzer(webp webp_fuzzer.cpp image_properties.hpp)
if(TARGET vnmaki_fixtures_webp)
  add_dependencies(fuzz_webp vnmaki_fixtures_webp)
  set(vnmaki_webp_corpus "${CMAKE_CURRENT_BINARY_DIR}/corpus/webp")
  file(MAKE_DIRECTORY "${vnmaki_webp_corpus}")
  add_test(NAME fuzz.webp.seeded
           COMMAND fuzz_webp -runs=10000 -seed=1 "${vnmaki_webp_corpus}"
                   "${PROJECT_BINARY_DIR}/fixtures/webp/seeds")
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
# Audio (ticket 16): the decoders and the audio snapshot. As for images, random bytes rarely
# make an audio file, so a second smoke run starts from the small clips among the audio fixtures.
vnmaki_add_fuzzer(audio audio_fuzzer.cpp)
vnmaki_add_fuzzer(audio_state audio_state_fuzzer.cpp)
if(TARGET vnmaki_fixtures_audio)
  add_dependencies(fuzz_audio vnmaki_fixtures_audio)
  set(vnmaki_audio_corpus "${CMAKE_CURRENT_BINARY_DIR}/corpus/audio")
  file(MAKE_DIRECTORY "${vnmaki_audio_corpus}")
  add_test(NAME fuzz.audio.seeded COMMAND fuzz_audio -runs=10000 -seed=1 "${vnmaki_audio_corpus}"
                                          "${PROJECT_BINARY_DIR}/fixtures/audio/seeds")
endif()

# Save loader (ticket 12). It edits a valid payload, so no seed corpus is needed.
vnmaki_add_fuzzer(save_loader save_loader_fuzzer.cpp)

# Movies (ticket 25): the WebM reader and the AV1 decoder. Random bytes never make a WebM file, so
# a second smoke run starts from the committed clip (a seed, never written to).
vnmaki_add_fuzzer(video video_fuzzer.cpp)
set(vnmaki_video_corpus "${CMAKE_CURRENT_BINARY_DIR}/corpus/video")
file(MAKE_DIRECTORY "${vnmaki_video_corpus}")
add_test(NAME fuzz.video.seeded COMMAND fuzz_video -runs=10000 -seed=1 "${vnmaki_video_corpus}"
                                        "${PROJECT_SOURCE_DIR}/tests/fixtures/video")

# Lua scan (ticket 34). Random bytes rarely form a label call, so a dictionary gives libFuzzer the
# pieces.
vnmaki_add_fuzzer(script_scan script_scan_fuzzer.cpp script_scan_properties.hpp)
add_test(NAME fuzz.script_scan.dictionary
         COMMAND fuzz_script_scan -runs=10000 -seed=1
                 "-dict=${CMAKE_CURRENT_SOURCE_DIR}/script_scan.dict")

# `.po` files (ticket 40). Random bytes rarely form a message, so a dictionary gives libFuzzer the
# pieces.
vnmaki_add_fuzzer(po po_fuzzer.cpp po_properties.hpp)
add_test(NAME fuzz.po.dictionary
         COMMAND fuzz_po -runs=10000 -seed=1 "-dict=${CMAKE_CURRENT_SOURCE_DIR}/po.dict")

# glTF models (ticket 46). Random bytes never make a glTF file, so a second smoke run starts from
# the generated cubes (seeds, never written to).
vnmaki_add_fuzzer(model model_fuzzer.cpp model_properties.hpp)
if(TARGET vnmaki_fixtures_model3d)
  add_dependencies(fuzz_model vnmaki_fixtures_model3d)
  set(vnmaki_model_corpus "${CMAKE_CURRENT_BINARY_DIR}/corpus/model")
  file(MAKE_DIRECTORY "${vnmaki_model_corpus}")
  add_test(NAME fuzz.model.seeded COMMAND fuzz_model -runs=10000 -seed=1 "${vnmaki_model_corpus}"
                                          "${PROJECT_BINARY_DIR}/fixtures/model3d/game/models")
endif()
