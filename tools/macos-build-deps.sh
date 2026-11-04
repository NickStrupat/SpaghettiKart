#!/bin/bash
# Builds SDL2 and libzip from source into build-deps/, for the macOS version the game targets.
#
# Homebrew ships its libraries built for the macOS it runs on, and the .app bundles them. Homebrew's SDL2 (a shim over
# SDL3) and libzip require the newest macOS, so an app built against them does not start on older ones. Build these
# two once, then configure the game against them:
#
#   tools/macos-build-deps.sh
#   cmake -S . -B build-cmake -DSDL2_DIR="$PWD/build-deps/prefix/lib/cmake/SDL2" \
#       -Dlibzip_DIR="$PWD/build-deps/prefix/lib/cmake/libzip"
#
# The other Homebrew libraries the app bundles (GLEW, ogg, vorbis, tinyxml2, xz, zstd) are built for macOS 15.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
DEPS="$REPO/build-deps"
PREFIX="$DEPS/prefix"
# Must match CMAKE_OSX_DEPLOYMENT_TARGET in CMakeLists.txt.
TARGET=15.0
SDL2_VERSION=2.32.10
LIBZIP_VERSION=1.12.1

# build <name> <url> [cmake options...]
build() {
    local name=$1 url=$2
    shift 2
    mkdir -p "$DEPS/src"
    if [ ! -f "$DEPS/$name.tar.gz" ]; then
        curl -fL --retry 3 -o "$DEPS/$name.tar.gz" "$url"
    fi
    if [ ! -d "$DEPS/src/$name" ]; then
        tar -xzf "$DEPS/$name.tar.gz" -C "$DEPS/src"
    fi
    cmake -S "$DEPS/src/$name" -B "$DEPS/build-$name" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$TARGET" -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_INSTALL_NAME_DIR="$PREFIX/lib" "$@"
    cmake --build "$DEPS/build-$name" --parallel
    cmake --install "$DEPS/build-$name"
}

build "SDL2-$SDL2_VERSION" \
    "https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VERSION/SDL2-$SDL2_VERSION.tar.gz" \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
build "libzip-$LIBZIP_VERSION" \
    "https://github.com/nih-at/libzip/releases/download/v$LIBZIP_VERSION/libzip-$LIBZIP_VERSION.tar.gz" \
    -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_OSSFUZZ=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF \
    -DENABLE_GNUTLS=OFF -DENABLE_MBEDTLS=OFF -DENABLE_OPENSSL=OFF

echo "Built into $PREFIX for macOS $TARGET"
