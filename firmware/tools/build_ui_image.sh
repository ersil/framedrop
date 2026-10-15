#!/bin/sh
# Pack firmware/data into the LittleFS image for the spiffs partition
# (0x2B0000, 0x140000 bytes in partitions_printdrop_ota.csv) and print the
# esptool command that writes it.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
mk="$here/../../.tools/mklittlefs"
[ -x "$mk" ] || "$here/build_mklittlefs.sh"
out="$here/../.pio/littlefs.bin"
mkdir -p "$(dirname "$out")"
"$mk" -c "$here/../data" -p 256 -b 4096 -s 0x140000 "$out" >/dev/null
"$mk" -l -p 256 -b 4096 -s 0x140000 "$out"
echo "write with: esptool --chip esp32s3 --port <port> write-flash 0x2B0000 \"$out\""
