# SD library (patched)

A copy of the SD library from the Arduino-ESP32 core used by this build
(framework-arduinoespressif32 4.20017.260907, Arduino core 2.0.17).
PlatformIO builds a library in the project's `lib/` folder in place of the
framework's copy of the same name.

The only change is in `src/sd_diskio.cpp`: card start-up no longer sends
ACMD42 (APP_CLR_CARD_DETECT). With it, a 3.3 V SDHC card on SPI completed
start-up, then failed the first block read (CMD9), and ignored every command
until it was power-cycled. Raw block reads without ACMD42 were clean.

A second change fixes a use-after-free in `sdcard_uninit()`: the
`AcquireSPI` guard's destructor dereferenced the card after `free(card)`. It
ran on every `SD.end()` and worked only because the freed block had not been
reused yet. clang-analyzer reported it.
