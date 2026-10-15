# OTA — Over-the-Air Update

PrintDrop uses a dual-OTA layout so a failed flash does not brick the stick. The single-app `partitions_printdrop.csv` cannot safely update itself.

## Partitions

`partitions_printdrop_ota.csv:1` (used by `env:printdrop` `platformio.ini:23`):

```
nvs      0x9000  20 KB
otadata  0xE000   8 KB
app0     0x10000 1 344 KB  ota_0
app1     0x160000 1 344 KB ota_1
spiffs   0x2B0000 1 280 KB LittleFS
coredump 0x3F0000   64 KB
```

Each slot holds ~1 344 KB; current firmware is ~980 KB `pio run -e printdrop` so it fits with headroom. `app0` is `ota_0`, `app1` `ota_1`; `otadata` tracks the boot slot. `printdrop_spi` keeps the old single-app table for bring-up without OTA.

Disable OTA on tight builds: `-D PRINTDROP_ENABLE_OTA=0` `config.h:135`.

## HTTP OTA

`POST /api/ota` (requires auth) streams a raw `.bin` to `Update` (`src/printdrop/ota.cpp:44`, `web.cpp:400`).

**Web UI** `Settings → Firmware`: choose a `.bin` (built via `pio run -e printdrop -t build` → `.pio/build/printdrop/firmware.bin`). The UI posts as `multipart/form-data` to `/api/ota` and shows progress; success reboots.

**curl:**
```bash
curl -u "admin:$PHOTOFRAME_WEB_PASSWORD" -F "file=@.pio/build/photoframe/firmware.bin" http://photoframe.local/api/ota
```

The web UI lives in the LittleFS partition, which a firmware update does not
touch. `?target=ui` writes the UI image from `tools/build_ui_image.sh`
instead; the device unmounts LittleFS first and reboots afterwards:

```bash
curl -u "admin:$PHOTOFRAME_WEB_PASSWORD" -F "file=@.pio/littlefs.bin" "http://photoframe.local/api/ota?target=ui"
```

**Serial** alternative is still `pio run -e printdrop -t upload` over `COM11`/`COM5` with the BOOT hatch `docs/flashing.md`.

## SD-card OTA

Copy to the SD root (FAT32):

```
firmware.bin              # same file as above
firmware.json             # {"version":"0.2.0","notes":"fix xyz"}
```

On boot, on `GET /api/ota/status`, and at most once a minute from `GET /api/status`, `ota::checkSD()` compares `firmware.json:version` against `PRINTDROP_VERSION`. If newer, the UI shows **SD update available: 0.2.0** with `Update from SD`. `POST /api/ota/sd` streams the file via `Update` and reboots.

## Rollback

`main.cpp` defines `verifyRollbackLater()` to return true, so Arduino does not mark a freshly updated image valid before `setup()`. `ota::markRunningValid()` marks it once it has run 60 seconds, and before any restart the firmware chooses itself (an OTA, `/api/restart`, a Wi-Fi save, a Wi-Fi watchdog restart). If the new image resets before either, for example by crashing in `setup()`, the bootloader boots the previous slot. Without the second rule, the restart after a web UI update rolled a good firmware update back.

Version is a plain string compare; use semver so `0.2.0` ≠ `0.1.0`.

## Safety

* Update is guarded by `storage::Guard` and `hostWrote` logic; the card is remounted after.
* `Update.begin(total)` validates size against the OTA slot before writing; power loss during flash leaves the other slot bootable.
* Factory reset (button 5 s, serial `factory-reset`, or `clear-auth` + Wi-Fi `forget`) does not touch OTA slots.
