# SD library (patched)

A copy of the SD library from the Arduino-ESP32 core used by this build
(framework-arduinoespressif32 4.20017.260907, Arduino core 2.0.17).
PlatformIO builds a library in the project's `lib/` folder in place of the
framework's copy of the same name. Usage is unchanged; see the
[upstream library](https://github.com/espressif/arduino-esp32/tree/2.0.17/libraries/SD).

There are two changes, both in `src/sd_diskio.cpp`:

- Card start-up no longer sends ACMD42 (APP_CLR_CARD_DETECT). With it, a
  3.3 V SDHC card on SPI completed start-up, then failed the first block read
  (CMD9), and ignored every command until it was power-cycled. Raw block reads
  without ACMD42 were clean.
- `sdcard_uninit()` no longer uses the card after freeing it. The
  `AcquireSPI` guard's destructor dereferenced the card after `free(card)` on
  every `SD.end()`. clang-analyzer reported it.
