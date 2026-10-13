#!/usr/bin/env bash
# Builds the SDL_shadercross command-line compiler into PREFIX/bin, with DXC and SPIRV-Cross
# built from shadercross's vendored submodules and SDL linked statically.
#
# Usage: build.sh PREFIX
#
# The commits match vcpkg's sdl3-shadercross port (3.0.0-preview2) and sdl3 port (3.4.18) at the
# manifest baseline, so every platform compiles shaders with the same shadercross.
set -euo pipefail

PREFIX=$1
SHADERCROSS_COMMIT=1ca46e0ef7a9e50c706e7be6ef73ce467bac3b2e
SDL_TAG=release-3.4.18
WORK=$(mktemp -d)
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc)

git clone --quiet --depth 1 --branch "$SDL_TAG" https://github.com/libsdl-org/SDL "$WORK/sdl"
cmake -S "$WORK/sdl" -B "$WORK/sdl-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$WORK/sdl-install" -DSDL_SHARED=OFF -DSDL_STATIC=ON \
  -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cmake --build "$WORK/sdl-build" --parallel "$JOBS"
cmake --install "$WORK/sdl-build"

git init --quiet "$WORK/shadercross"
git -C "$WORK/shadercross" remote add origin https://github.com/libsdl-org/SDL_shadercross
git -C "$WORK/shadercross" fetch --quiet --depth 1 origin "$SHADERCROSS_COMMIT"
git -C "$WORK/shadercross" checkout --quiet FETCH_HEAD
git -C "$WORK/shadercross" submodule update --init --recursive --depth 1 --jobs 4

cmake -S "$WORK/shadercross" -B "$WORK/shadercross-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$WORK/sdl-install" -DSDLSHADERCROSS_VENDORED=ON \
  -DSDLSHADERCROSS_DXC=ON -DSDLSHADERCROSS_SHARED=OFF -DSDLSHADERCROSS_STATIC=ON \
  -DSDLSHADERCROSS_CLI=ON -DSDLSHADERCROSS_CLI_STATIC=ON -DSDLSHADERCROSS_INSTALL=OFF
cmake --build "$WORK/shadercross-build" --target shadercross --parallel "$JOBS"

# Keep only the compiler and the DXC libraries it loads, next to each other.
mkdir -p "$PREFIX/bin"
cp "$WORK/shadercross-build/shadercross" "$PREFIX/bin/"
find "$WORK/shadercross-build" \( -name 'libdxcompiler*.dylib' -o -name 'libdxil*.dylib' \) \
  -exec cp -P {} "$PREFIX/bin/" \;
install_name_tool -add_rpath @executable_path "$PREFIX/bin/shadercross" 2>/dev/null || true
"$PREFIX/bin/shadercross" --help > /dev/null 2>&1 || {
  otool -L "$PREFIX/bin/shadercross"
  echo "shadercross does not run" >&2
  exit 1
}
rm -rf "$WORK"
