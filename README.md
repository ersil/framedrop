# FrameDrop

Wi-Fi photo uploads for a digital photo frame that only reads USB drives.

An ESP32-S3 SuperMini plugs into the frame's USB port. The frame sees a USB
flash drive backed by a microSD card. The S3 joins your Wi-Fi and serves a web
page at `http://photoframe.local` where you add photos from a phone or
computer, and the frame shows them without stopping its slideshow. An iOS
Shortcut sends photos straight from the iPhone share sheet.

It has been tested with a 2018 NIX digital photo frame.

## About this project

AI was used heavily to build this project. Most of the firmware changes, the
web page, the iOS Shortcut and the documentation were written with an AI coding
assistant, directed by a person and tested on the real frame. The code has been
through several rounds of AI security review and code review, and the fixes
from each round are in the code.

It is meant for a home network. The device serves plain HTTP with Basic auth,
and its upload progress WebSocket has no login (see [Security](#security)). Do
not expose it to the internet.

## What you need

- An ESP32-S3 SuperMini (4 MB flash).
- A 3.3 V microSD breakout with pull-ups on the data lines, such as Adafruit
  4682. Avoid modules with an AMS1117 regulator: they expect 5 V, and on 3.3 V
  the card gets too little voltage to answer.
- A microSD card formatted FAT32. Cards over 32 GB ship as exFAT, which the
  firmware does not read; on a Mac, reformat them with Disk Utility's MS-DOS
  (FAT) format.
- Six short wires, and a soldering iron for the S3's header pins.

## Wiring

| SuperMini | Breakout |
|---|---|
| 3V3 | 3V |
| GND | GND |
| GPIO 4 | CLK |
| GPIO 5 | SI (MOSI) |
| GPIO 6 | SO (MISO) |
| GPIO 7 | CS |

Keep the wires short, with the clock wire shortest.

## Install

### 1. Install the tools

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html).
Building the web page image also needs `clang` and `git`.

On a Mac with [Homebrew](https://brew.sh) (optional):

```sh
# Homebrew's installer already set up clang and git
brew install platformio
```

### 2. Choose the settings

The build reads its settings from environment variables and stops if a
required one is missing. The firmware has no default passwords. Set them in the
shell you build from:

```sh
export PHOTOFRAME_WEB_PASSWORD='choose-a-password'
export PHOTOFRAME_SETUP_PASSWORD='choose-another'
```

| Variable | Meaning |
|---|---|
| `PHOTOFRAME_WEB_PASSWORD` | Required. Password for the web page, user `admin`. |
| `PHOTOFRAME_SETUP_PASSWORD` | Required. Password of the `PhotoFrame-Setup` Wi-Fi network the device opens when it has no Wi-Fi to join. 8 to 63 characters. |
| `PHOTOFRAME_WIFI_SSID` | Optional. Your Wi-Fi network, so the device joins it on first boot without the setup network. |
| `PHOTOFRAME_WIFI_PASSWORD` | Optional. Its password. Leave it empty for an open network. |
| `PHOTOFRAME_TZ` | Optional. Time zone for file dates, as a POSIX TZ string such as `PST8PDT,M3.2.0,M11.1.0` or `CET-1CEST,M3.5.0,M10.5.0/3`. Default `UTC0`. |

### 3. Check the wiring

Plug the S3 into your computer with the card inserted. All commands run from
the `firmware` folder.

```sh
cd firmware
pio run -e photoframe_sd_diag -t upload
pio device monitor
```

Press a key to start the report. It checks each wire and the card, and changes
nothing on the card. If it reports a problem, see
[The wiring check reports a problem](#the-wiring-check-reports-a-problem).

### 4. Flash the firmware

```sh
tools/build_ui_image.sh
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port <port> \
  write_flash 0x2B0000 .pio/littlefs.bin
pio run -e photoframe -t upload
```

The first command builds the web page image. The first time, it also builds
the `mklittlefs` tool from source into `.tools/`. The second writes the image;
`pio device list` shows the port (`/dev/cu.usbmodem…` on a Mac). The third
builds and writes the firmware.

When it finishes, unplug the S3 and plug it back in. Once the firmware finds
the card, the S3's USB port becomes the drive and no longer accepts commands
from your computer, so later updates go [over Wi-Fi](#updating). To flash it
over USB anyway, hold BOOT while pressing RESET.

### 5. Connect it to your Wi-Fi

If you set `PHOTOFRAME_WIFI_SSID`, the device joins that network on its own.

Otherwise, join the `PhotoFrame-Setup` network with the setup password and
open `http://192.168.4.1`. Log in with the web password, go to
**Settings → Wi-Fi**, choose your network, enter its password and press
**Save and restart**.

### 6. Plug it into the frame

Plug the S3 into the frame's USB port and set the frame to play from its USB
drive. Open `http://photoframe.local`, add a photo, and check that it appears
on the frame.

## Adding photos

Open `http://photoframe.local` on a phone or computer on the same Wi-Fi and
enter the web password. On the **Add** tab, press **Choose photos**, or drop
photos onto the page on a computer. Each photo is shrunk to 1920 px on its long
edge and saved as a JPEG before it is sent. The frame keeps playing while
photos upload and picks up the new ones once the last upload finishes.

The **Photos** tab lists what is on the card. Select photos there to delete
them.

To send photos from an iPhone's share sheet, install the Shortcut in
[`shortcut/`](shortcut/).

If `photoframe.local` does not open, use the device's IP address, which your
router lists.

## Updating

Firmware and web page updates go over Wi-Fi. Each one restarts the device, and
the frame sees the drive plugged in again.

```sh
cd firmware
pio run -e photoframe
curl -u "admin:$PHOTOFRAME_WEB_PASSWORD" -F "file=@.pio/build/photoframe/firmware.bin" http://photoframe.local/api/ota
tools/build_ui_image.sh
curl -u "admin:$PHOTOFRAME_WEB_PASSWORD" -F "file=@.pio/littlefs.bin" "http://photoframe.local/api/ota?target=ui"
```

**Settings → Firmware** in the web page also takes a firmware `.bin`.

New firmware has to run for 60 seconds before it is kept. If it crashes before
then, the device goes back to the previous version on its next start.

Keep a copy of `.pio/build/photoframe/firmware.elf` for each version you
install. You need the matching one to read a crash report.

## Troubleshooting

### The slideshow has stopped

Press **Restart slideshow** under **Settings → Frame**. The S3 restarts and the
frame sees the drive plugged in again. The NIX starts its slideshow when that
happens.

### The device is not on the Wi-Fi

It retries every minute and restarts after 10 minutes without Wi-Fi. If it
fell back to the `PhotoFrame-Setup` network while a network is saved, it
restarts after 10 minutes with nobody connected to try your network again.

To start over, hold BOOT for 5 seconds. That clears the Wi-Fi network and
login saved on the device, so the values from the build apply again, and
restarts it.

### The wiring check reports a problem

The check reads each line with the chip's pull-down on. A data line that reads
0 is not reaching the breakout: check that wire and its solder joints. CS reads
0 even when connected, which is normal.

If it reports that MISO floats, the card is not driving the line. Check that a
card is inserted, that the breakout's power pin gets 3.3 V, and that the SO
wire goes to GPIO 6.

If the card stops answering after a failed attempt, unplug the S3 to
power-cycle the card and run the check again.

### Reading logs and crash reports

- `http://photoframe.local/api/log` (needs the login) shows the last 4 KB of
  log lines. They survive a crash or watchdog reset.
- `http://photoframe.local/api/status` shows the firmware version, Wi-Fi,
  card and USB state, and why the device last restarted.
- `http://photoframe.local/api/crash` (needs the login) summarises the last
  crash: task, program counter, backtrace, and the SHA-256 of the firmware
  that crashed.
  Turn the addresses into source lines with the `firmware.elf` whose SHA-256
  matches:

  ```sh
  ~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-addr2line -pfiaC -e firmware.elf <addresses>
  ```

## Security

- Every action that reads, adds or deletes photos, changes settings or
  updates firmware needs the login. The page itself and the status endpoints
  (`/api/status`, `/api/auth/status`, `/api/ota/status`) do not.
- The login is HTTP Basic auth over plain HTTP, so anyone who can watch your
  Wi-Fi traffic can read it.
- The upload progress WebSocket on port 81 has no login. Anyone on your network
  who connects receives file names and the device status.
- Requests that another website's page sends are refused, so a page elsewhere
  cannot use your browser's saved login.
- The firmware stores only a SHA-256 of the web password.

## How it works

- The frame sees the card as a read-only drive, so it can never write to it
  while the web page is adding photos.
- Photos upload while the drive stays connected. When a batch of uploads ends,
  the S3 withdraws the card for about 2.3 seconds and signals the frame that
  the medium changed. The frame then reads the card again and finds the new
  photos without stopping.
- A batch opens with `POST /api/batch/begin` and ends with
  `POST /api/batch/end`. The web page and the Shortcut use these, so several
  photos refresh the frame once. A batch that is never ended closes after
  10 minutes without an upload. Uploads sent without a batch refresh the frame
  30 seconds after the last one. An upload that stops sending for 5 seconds is
  dropped and its partial file deleted.
- The firmware is a fork of [PrintDrop](https://github.com/Kabani-Tech/PrintDrop)
  (upstream commit `7544aaa`), a Wi-Fi USB drive for 3D printers. Its design
  notes, written for its own board, are in
  [upstream's docs](https://github.com/Kabani-Tech/PrintDrop/tree/7544aaa/docs).
- `firmware/lib/SD` and `firmware/lib/WebServer` are patched copies of the
  Arduino-ESP32 libraries. The SD patch skips a start-up command (ACMD42) that
  left some cards unresponsive until power-cycled. The WebServer patch limits
  request sizes and waits, so a client cannot hang or crash the device. Each
  folder's README lists the changes.

## License

MIT, see [`LICENSE`](LICENSE). The firmware is a fork of PrintDrop (MIT,
[`firmware/LICENSE`](firmware/LICENSE)). `firmware/lib/SD` is Espressif's SD
library (Apache-2.0) and `firmware/lib/WebServer` is the Arduino-ESP32
WebServer library (LGPL-2.1).
