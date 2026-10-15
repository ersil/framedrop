# Gate 1: flash-backed USB drive

Proof of concept, kept for reference. The S3 serves a 2 MiB FAT12 image from
its own flash to the USB host as a read-only drive. On 2026-10-12 the NIX frame
showed `TEST.JPG` from it, which proved the frame accepts the S3 as a USB drive.

Photos will not be stored in flash. The real firmware is the PrintDrop fork in
`../../firmware`.

Build from this folder with `arduino-cli` and the Arduino-ESP32 core 3.3.
`brew install mtools` provides `mformat` and `mcopy`.

```sh
tools/make_disk.sh disk/TEST.JPG
arduino-cli compile \
  --fqbn 'esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashSize=4M' \
  --output-dir build firmware/photoframe
esptool --chip esp32s3 --port "$(ls /dev/cu.usbmodem* | head -1)" write-flash \
  0x0 build/photoframe.ino.bootloader.bin 0x8000 build/photoframe.ino.partitions.bin \
  0x10000 build/photoframe.ino.bin 0x1F0000 disk/photos.img
```
