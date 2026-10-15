#!/bin/sh
# Build a native mklittlefs into .tools/mklittlefs at the repo root.
#
# PlatformIO's tool-mklittlefs and the Arduino core's copy are Intel-only
# binaries, which do not run on Apple Silicon without Rosetta. This compiles
# the same release PlatformIO pins (mklittlefs 3.0.0, littlefs v2.0.5, 32-char
# names), so the image matches what `pio run -t buildfs` would produce.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out="$here/../../.tools/mklittlefs"
mkdir -p "$(dirname "$out")"
src=$(mktemp -d)
obj=$(mktemp -d)
trap 'rm -rf "$src" "$obj"' EXIT

git clone -q --branch 3.0.0 --depth 1 --recurse-submodules \
  https://github.com/earlephilhower/mklittlefs.git "$src"

flags="-Os -D__NO_INLINE__ -DLFS_NAME_MAX=32"
cd "$obj"
clang -std=gnu99 $flags -I"$src/littlefs" -c "$src/littlefs/lfs.c" "$src/littlefs/lfs_util.c"
clang++ -std=gnu++11 $flags \
  -DVERSION='"3.0.0"' -DLITTLEFS_VERSION='"v2.0.5"' \
  -DBUILD_CONFIG='"native"' -DBUILD_CONFIG_NAME='"-native"' \
  -I"$src/include" -I"$src/littlefs" -I"$src/tclap" -I"$src" \
  "$src/main.cpp" lfs.o lfs_util.o -o "$out"
"$out" --version
