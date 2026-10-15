# FrameDrop

Wi-Fi photo uploads for a digital photo frame that only reads USB drives.

An ESP32-S3 SuperMini plugged into the USB port of a 2018 NIX digital photo
frame. The S3 appears to the frame as a read-only USB flash drive backed by a
microSD card, joins the home Wi-Fi, and serves a web page for uploading photos
at `http://photoframe.local`.

## About this project

AI was used heavily to build this project. Most of the firmware changes,
the web page, the iOS Shortcut and the documentation were written with an AI
coding assistant, directed by a person and tested on the real frame. The code
has been through several rounds of AI security review and code review, and the
fixes from each round are in the code.

It is meant for a home network. The device serves plain HTTP with Basic auth,
and its upload progress WebSocket has no login (see Security below). Do not
expose it to the internet.

## How it works

- **Firmware:** a fork of [PrintDrop](https://github.com/Kabani-Tech/PrintDrop)
  (MIT, upstream commit `7544aaa`), a Wi-Fi USB drive for 3D printers. It
  provides one lock for all SD access, a read-only USB side so the frame cannot
  write stale FAT data back, Wi-Fi with a setup network as fallback, mDNS, a
  password on the web UI, and OTA updates. Its license is in
  `firmware/LICENSE`.
- **Storage:** photos live on the microSD card only, never in the S3's flash.
- **Refresh:** the drive stays connected while photos upload, so the frame
  keeps playing. When a batch ends, the S3 withdraws the medium for about
  2.3 seconds, raises UNIT ATTENTION ("medium may have changed") and presents
  it again, and the frame picks up the new photos. This is
  `USB_FORCE_REATTACH=0`, the `photoframe` build's setting. With `1`, the S3
  leaves the USB bus for the whole batch instead.
- **Network:** the S3 joins the home Wi-Fi. The `PhotoFrame-Setup` network
  appears only when no Wi-Fi is saved or joining the saved one fails.

### Batches

A multi-file upload refreshes the frame once, after the last file. Any number
of files can go in a batch.

- `POST /api/batch/begin` opens a batch. It stays open until
  `POST /api/batch/end`, however long the uploads take.
- The upload page calls begin before its first file and end when its queue is
  empty. Files added while it uploads join the same batch. The iOS Shortcut in
  `shortcut/` does the same.
- If end never arrives, the batch closes after 10 minutes without a write.
- An upload that stops sending is abandoned after 5 seconds without a byte,
  and its partial file is deleted.
- Sending end when no batch is open does nothing.
- A write made without begin opens a batch that ends 30 seconds after the last
  write finishes. An upload in progress never counts toward that.

## Hardware

- ESP32-S3 SuperMini with 4 MB flash and 2 MB PSRAM.
- A 3.3 V-only microSD breakout with pull-ups on the data lines, such as
  Adafruit 4682. A module with an AMS1117 regulator expects 5 V; on 3.3 V the
  regulator leaves the card too little voltage to answer.
- A microSD card formatted FAT32.

### Wiring

The card runs over SPI at 20 MHz.

| SuperMini | Breakout |
|---|---|
| 3V3 | 3V |
| GND | GND |
| GPIO 4 | CLK |
| GPIO 5 | SI (MOSI) |
| GPIO 6 | SO (MISO) |
| GPIO 7 | CS |

GPIO 4 to 7 are not strapping pins (on the ESP32-S3 those are 0, 3, 45 and
46), and none of them is used by USB (19, 20) or the RGB LED (48). Keep the
wires short, with the clock wire shortest.

### The card

Format the card FAT32. Cards over 32 GB ship as exFAT, which the firmware does
not mount; reformat them with macOS Disk Utility's MS-DOS (FAT) format.

The firmware uses a patched copy of the core's SD library in
`firmware/lib/SD`. It skips ACMD42 during card start-up, which leaves some
cards unresponsive until power-cycled. `lib/SD/PHOTOFRAME.md`
describes the changes.

## Build and flash

The build needs [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html)
and reads its settings from environment variables. `tools/build_secrets.py`
writes them into a generated header in the build folder, never onto the
compiler command line. The build fails if a required variable is missing, and
the firmware has no default passwords. To keep them out of your shell history,
put them in a `.envrc` for [direnv](https://direnv.net) (Git ignores it) or
load them from a secret manager.

| Key | Used for |
|---|---|
| `PHOTOFRAME_WEB_PASSWORD` | Required. Web UI and OTA login, user `admin` unless changed on the Login tab. Only its SHA-256 is compiled in. A reflash applies a new value unless the password was changed on the device. |
| `PHOTOFRAME_SETUP_PASSWORD` | Required. WPA2 password of the `PhotoFrame-Setup` network, 8 to 63 printable ASCII characters. |
| `PHOTOFRAME_WIFI_SSID`, `PHOTOFRAME_WIFI_PASSWORD` | Optional. The home Wi-Fi the device joins when none is saved; leave the password empty for an open network. A network saved from the web page takes priority. |
| `PHOTOFRAME_TZ` | Optional. POSIX TZ string for file timestamps, such as `PST8PDT,M3.2.0,M11.1.0` or `CET-1CEST,M3.5.0,M10.5.0/3`. Default `UTC0`. |

```sh
cd firmware
pio run -e photoframe           # build
pio run -e photoframe -t upload # flash the firmware over USB
tools/build_ui_image.sh         # pack data/ into .pio/littlefs.bin
```

`pio run -t uploadfs` does not work on Apple Silicon, because PlatformIO's
`mklittlefs` is an Intel-only binary. `tools/build_ui_image.sh` uses a native
build in `.tools/mklittlefs` at the repo root, which `tools/build_mklittlefs.sh`
builds from source on first use (mklittlefs 3.0.0, the release PlatformIO pins). Write the image
to `0x2B0000` with esptool, as the script prints.

### Updating over Wi-Fi

Once a card mounts, the USB port is the drive and there is no serial console.
Update over Wi-Fi instead, firmware and web UI separately. Each one restarts
the device.

```sh
curl -u "admin:$PHOTOFRAME_WEB_PASSWORD" -F "file=@.pio/build/photoframe/firmware.bin" http://photoframe.local/api/ota
curl -u "admin:$PHOTOFRAME_WEB_PASSWORD" -F "file=@.pio/littlefs.bin" "http://photoframe.local/api/ota?target=ui"
```

After an update the new firmware is on probation. It is marked valid once it
has run for 60 seconds, or before any restart it makes itself. If it resets
before then, the bootloader boots the previous firmware.

Keep the `firmware.elf` of every build you flash, to decode crash backtraces
later.

### Flashing over USB

With no card inserted, the firmware never starts its own USB device, so the
chip's serial/JTAG port stays up and esptool can flash it directly. With a
card inserted, hold BOOT while pressing RESET to enter download mode. After
flashing, unplug and replug the board, because a reset from esptool leaves it
in download mode.

### First setup

1. Flash `photoframe_sd_diag`, open `pio device monitor`, and press a key
   to start the report. It checks the wiring and the card without changing
   anything. Pressing `A` also runs the ACMD42 check, which leaves the card
   unresponsive until it is power-cycled.
2. Flash `photoframe` and the UI image.
3. With `PHOTOFRAME_WIFI_SSID` and `PHOTOFRAME_WIFI_PASSWORD` set, the device
   joins the home Wi-Fi on its own. Without them, join `PhotoFrame-Setup`
   (password: `PHOTOFRAME_SETUP_PASSWORD`), open `http://192.168.4.1`, and
   save the home Wi-Fi.
4. Plug the S3 into the frame, upload a photo from `http://photoframe.local`,
   and check that it appears once the batch ends.

### Reading the diagnostic

The diagnostic reads each of the four lines with the chip's pull-down on and
checks that no two lines are bridged. A line that reads 1 is wired to the
powered breakout or card, which pull their data lines up. CS reads 0 even when
connected, because the card's pull-up is weak. A data line that reads 0 is
not reaching the breakout, so check that wire and its solder joints.

If it reports that MISO floats, the card is not driving the line. Check that
a card is inserted, that the breakout's power pin gets 3.3 V, and that the SO
wire goes to GPIO 6.

If the card stops answering after a failed mount, unplug the S3 to power-cycle
the card before running the diagnostic again.

## Using it

The web page asks only for the password and takes the username from the
device. Once on Wi-Fi, the device sets its clock over NTP in the zone set by
`PHOTOFRAME_TZ`, so uploaded files carry real timestamps.

A short press of the BOOT button ends the open batch, or refreshes the frame
if none is open. Holding it for 5 seconds clears the Wi-Fi network and login
saved on the device, so the build's values apply again, and restarts it.

### When something goes wrong

- **The slideshow has stopped:** press **Restart slideshow** on the Frame
  settings tab (`POST /api/restart`). The S3 restarts, the frame sees the drive
  plugged in again, and the frame starts its slideshow.
- **The device is off the home Wi-Fi:** it asks the driver to reconnect every
  minute and restarts after 10 minutes. If it fell back to `PhotoFrame-Setup`
  while a home network is saved, it restarts to try again after 10 minutes
  with nobody on the setup network.
- **Logs:** `GET /api/log` returns the last 4 KB of log lines. They are kept
  in RTC memory, which survives a crash or watchdog reset. `/api/status`
  reports the last reset reason.
- **Crashes:** `GET /api/crash` summarises the core dump the last panic left:
  task, PC and backtrace, and the ELF's SHA-256. Decode the addresses with
  `~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-addr2line -pfiaC -e firmware.elf`,
  using the ELF whose hash matches.

## Security

- Every API call that reads the card, changes it, uploads or changes settings
  needs the login. The page itself and the status endpoints (`/api/status`,
  `/api/auth/status`, `/api/ota/status`) do not.
- Requests whose `Origin` names another site are refused, so a web page
  elsewhere cannot use a browser's saved login to drive the device.
- The WebSocket on port 81 (upload progress) has no login. Anyone on the home
  network who connects receives file names and the status JSON.
- The web UI is served with `Cache-Control: no-cache`, so a phone loads a
  reflashed UI on its next visit.
- `firmware/lib/WebServer` is a patched copy of the core's WebServer that
  bounds request sizes and waits, so a client cannot hang or crash the device
  before the login check. `lib/WebServer/PHOTOFRAME.md` lists the changes.

## License

MIT, see `LICENSE`. The firmware is a fork of PrintDrop (MIT, `firmware/LICENSE`).
`firmware/lib/SD` is Espressif's SD library (Apache-2.0) and
`firmware/lib/WebServer` is the Arduino-ESP32 WebServer library (LGPL-2.1),
each with local changes described in its `PHOTOFRAME.md`.
