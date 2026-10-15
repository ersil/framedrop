#!/bin/sh
# Build the FAT image for the "photos" partition (2 MiB, superfloppy, no MBR)
# from the JPEGs given as arguments.
set -eu
[ "$#" -gt 0 ] || { echo "usage: $0 photo.jpg [photo.jpg ...]" >&2; exit 1; }
for f in "$@"; do [ -f "$f" ] || { echo "not a file: $f" >&2; exit 1; }; done
out="$(dirname "$0")/../disk/photos.img"
mkdir -p "$(dirname "$out")"
rm -f "$out"
dd if=/dev/zero of="$out" bs=512 count=4096
mformat -i "$out" -T 4096 -h 2 -s 32 -v PHOTOS ::
# -D o overwrites instead of prompting when two inputs share a name.
for f in "$@"; do mcopy -D o -i "$out" "$f" ::; done
mdir -i "$out" ::
