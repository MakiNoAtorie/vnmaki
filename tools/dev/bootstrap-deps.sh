#!/usr/bin/env bash
# Builds the engine's dependencies from git tags into a prefix, for Linux containers where vcpkg
# cannot download (GitHub source archives are refused; git clone and release assets work).
#
# Usage: bootstrap-deps.sh [PREFIX]     (default /opt/vnmaki-deps)
#
# Then build with a system-* preset:
#   CC=clang CXX=clang++ CMAKE_PREFIX_PATH=/opt/vnmaki-deps cmake --workflow --preset system-debug
#
# Versions match vcpkg.json at its builtin-baseline. Change them together.
set -euo pipefail

PREFIX=${1:-/opt/vnmaki-deps}
SDL_TAG=release-3.4.18
SOL2_TAG=v3.5.0
YOGA_TAG=v3.2.1
SHEENBIDI_TAG=v3.0.0
SPIRV_CROSS_TAG=vulkan-sdk-1.4.363.0
SHADERCROSS_COMMIT=1ca46e0ef7a9e50c706e7be6ef73ce467bac3b2e
DXC_URL=https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2602.24/linux_dxc_2026_05_26.x86_64.tar.gz

# A finished prefix is kept until this script changes.
STAMP="$PREFIX/.bootstrap-$(sha256sum "$0" | cut -c1-16)"
if [[ -e $STAMP ]]; then
  exit 0
fi

SUDO=""
if [[ $(id -u) -ne 0 ]]; then
  SUDO=sudo
fi
$SUDO apt-get update -qq
$SUDO apt-get install -y -qq --no-install-recommends \
  git cmake ninja-build pkg-config curl ca-certificates \
  doctest-dev liblua5.4-dev libtomlplusplus-dev libclang-rt-18-dev libstb-dev \
  libharfbuzz-dev libfreetype-dev libunibreak-dev \
  mesa-vulkan-drivers libvulkan1 \
  libx11-dev libxext-dev libxtst-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev \
  libxss-dev libwayland-dev libxkbcommon-dev libegl1-mesa-dev libasound2-dev libpulse-dev
$SUDO mkdir -p "$PREFIX"
$SUDO chown "$(id -u):$(id -g)" "$PREFIX"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# build NAME REPO REF [cmake options...]: clones one tag and installs it into PREFIX.
build() {
  local name=$1 repo=$2 ref=$3
  shift 3
  git clone --quiet --depth 1 --branch "$ref" "https://github.com/$repo" "$WORK/$name"
  if [[ $name == yoga ]]; then
    # The same change as vcpkg's disable_tests.patch.
    sed -i 's/^add_subdirectory(tests)/# &/' "$WORK/$name/CMakeLists.txt"
  fi
  cmake -S "$WORK/$name" -B "$WORK/$name-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON "$@" > /dev/null
  cmake --build "$WORK/$name-build" > /dev/null
  cmake --install "$WORK/$name-build" > /dev/null
}

build sdl libsdl-org/SDL "$SDL_TAG" -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
build sol2 ThePhD/sol2 "$SOL2_TAG"
build yoga facebook/yoga "$YOGA_TAG"
build sheenbidi Tehreer/SheenBidi "$SHEENBIDI_TAG" -DSB_CONFIG_UNITY=ON
build spirv-cross KhronosGroup/SPIRV-Cross "$SPIRV_CROSS_TAG" \
  -DSPIRV_CROSS_ENABLE_C_API=ON -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_CLI=OFF

# DXC is a prebuilt release: bin/dxc, lib/libdxcompiler.so, lib/libdxil.so, include/dxc/.
curl -fsSL "$DXC_URL" | tar -xz -C "$PREFIX"

# shadercross has no tag at this version, so fetch the commit.
git init --quiet "$WORK/shadercross"
git -C "$WORK/shadercross" fetch --quiet --depth 1 \
  https://github.com/libsdl-org/SDL_shadercross "$SHADERCROSS_COMMIT"
git -C "$WORK/shadercross" checkout --quiet FETCH_HEAD
cmake -S "$WORK/shadercross" -B "$WORK/shadercross-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DDirectXShaderCompiler_ROOT="$PREFIX" -DCMAKE_INSTALL_RPATH="$PREFIX/lib" \
  -DSDLSHADERCROSS_VENDORED=OFF -DSDLSHADERCROSS_SPIRVCROSS_SHARED=OFF \
  -DSDLSHADERCROSS_SHARED=OFF -DSDLSHADERCROSS_STATIC=ON \
  -DSDLSHADERCROSS_CLI=ON -DSDLSHADERCROSS_CLI_STATIC=ON -DSDLSHADERCROSS_INSTALL=ON > /dev/null
cmake --build "$WORK/shadercross-build" > /dev/null
cmake --install "$WORK/shadercross-build" > /dev/null
"$PREFIX/bin/shadercross" --help > /dev/null

touch "$STAMP"
