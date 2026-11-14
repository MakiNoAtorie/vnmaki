# Text module: fonts, paragraph layout, the glyph atlas and typewriter reveal (docs/SPEC.md
# section 13).
target_sources(vnmaki_engine PRIVATE markup.cpp markup.hpp text.cpp text.hpp)

find_package(harfbuzz CONFIG REQUIRED)
# FreeType installs no CMake config on Debian/Ubuntu, so this is CMake's FindFreetype (as FindLua
# is used for Lua); vcpkg wraps the same call.
find_package(Freetype REQUIRED)
# libunibreak has a CMake config only in vcpkg, so find its files directly.
find_path(VNMAKI_UNIBREAK_INCLUDE_DIR linebreak.h REQUIRED)
find_library(VNMAKI_UNIBREAK_LIBRARY unibreak REQUIRED)
target_include_directories(vnmaki_engine SYSTEM PRIVATE "${VNMAKI_UNIBREAK_INCLUDE_DIR}")
# SheenBidi gives the bidi embedding levels (right-to-left and mixed text).
find_package(SheenBidi CONFIG REQUIRED)
target_link_libraries(vnmaki_engine PRIVATE harfbuzz::harfbuzz Freetype::Freetype
                                            "${VNMAKI_UNIBREAK_LIBRARY}" SheenBidi::SheenBidi)
