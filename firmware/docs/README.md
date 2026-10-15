# Firmware documentation

These pages come from upstream PrintDrop and describe its own hardware: an
ESP32-S3-DevKitC-1 with the card on SDIO. FrameDrop runs the same firmware on
an ESP32-S3 SuperMini with a microSD breakout on SPI, so the pin, wiring and
board details here do not apply to it. The [project README](../../README.md)
covers FrameDrop's hardware, build and use. `architecture.md`, `hardware.md`
and `flashing.md` start with a note on what differs.

| Document | What's in it |
|---|---|
| [architecture.md](architecture.md) | How USB and Wi-Fi share the card, why the card is offered read-only, module layout, partition map, build environments |
| [auth.md](auth.md) | The web login: where credentials live and how they are set |
| [bugs.md](bugs.md) | Faults found during upstream's port and their root causes |
| [discovery.md](discovery.md) | How the device is found on the network (mDNS and LLMNR) |
| [flashing.md](flashing.md) | Getting the board into download mode |
| [hardware.md](hardware.md) | Upstream's board, SD wiring and power |
| [ota.md](ota.md) | The dual-slot OTA layout |
| [sdio.md](sdio.md) | Upstream's SDIO card driver, which FrameDrop does not use |
