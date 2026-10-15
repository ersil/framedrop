// SD card diagnostic firmware for ESP32-S3-DevKitC-1.
// Leaves USB in Serial/JTAG mode so the board stays flashable while testing.

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <driver/gpio.h>
#include "../common/reflash_hatch.h"

#ifndef SD_CS_PIN
#define SD_CS_PIN   12
#endif
#ifndef SD_MISO_PIN
#define SD_MISO_PIN 39
#endif
#ifndef SD_MOSI_PIN
#define SD_MOSI_PIN 14
#endif
#ifndef SD_CLK_PIN
#define SD_CLK_PIN  40
#endif

SPIClass sdSPI;

// Mirror every line to the USB/JTAG console and to UART0, so the board can be
// watched from whichever of the two cables the user has open.
static void out(const String& s) {
    Serial.println(s);
#if ARDUINO_USB_CDC_ON_BOOT
    Serial0.println(s);
#endif
}

static void outf(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    out(String(buf));
}

static void banner(const char* title) {
    out("");
    out("==================================================");
    out(String("  ") + title);
    out("==================================================");
}

static void reportChip() {
    banner("CHIP / BOARD");
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint32_t flashSize = 0;
    esp_flash_get_size(NULL, &flashSize);

    outf("Chip model    : %d (cores=%d, rev=%d)", (int)info.model, info.cores, info.revision);
    outf("Flash size    : %u bytes (%u MB)", flashSize, flashSize / (1024 * 1024));
    outf("PSRAM size    : %u bytes", (unsigned)ESP.getPsramSize());
    outf("Free heap     : %u bytes", (unsigned)ESP.getFreeHeap());
    outf("SDK version   : %s", ESP.getSdkVersion());
#ifdef ARDUINO_USB_MODE
    outf("ARDUINO_USB_MODE        : %d", ARDUINO_USB_MODE);
#else
    out("ARDUINO_USB_MODE        : (undefined)");
#endif
    outf("ARDUINO_USB_CDC_ON_BOOT : %d", (int)ARDUINO_USB_CDC_ON_BOOT);
}

// --- Low level SPI probe -----------------------------------------------------
// Talks raw SD SPI-mode commands so we can tell "nothing is wired up" apart from
// "card answers but the Arduino SD driver refuses to mount it".

static uint8_t sdCommand(uint8_t cmd, uint32_t arg, uint8_t crc) {
    sdSPI.transfer(0x40 | cmd);
    sdSPI.transfer((arg >> 24) & 0xFF);
    sdSPI.transfer((arg >> 16) & 0xFF);
    sdSPI.transfer((arg >> 8) & 0xFF);
    sdSPI.transfer(arg & 0xFF);
    sdSPI.transfer(crc);
    // R1 arrives within 8 polls; 0xFF means the card is still busy / silent.
    for (int i = 0; i < 10; ++i) {
        uint8_t r = sdSPI.transfer(0xFF);
        if (!(r & 0x80)) return r;
    }
    return 0xFF;
}

static void probePins() {
    banner("PIN CONTINUITY PROBE");
    outf("Configured pins: CS=%d MISO=%d MOSI=%d CLK=%d",
         SD_CS_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CLK_PIN);

    // A wired, powered card holds MISO high (open-drain + pull-up) when idle.
    pinMode(SD_MISO_PIN, INPUT_PULLUP);
    delay(5);
    int misoPullup = digitalRead(SD_MISO_PIN);
    pinMode(SD_MISO_PIN, INPUT_PULLDOWN);
    delay(5);
    int misoPulldown = digitalRead(SD_MISO_PIN);
    pinMode(SD_MISO_PIN, INPUT);

    outf("MISO with pull-up   : %d (expect 1)", misoPullup);
    outf("MISO with pull-down : %d", misoPulldown);
    if (misoPullup == 1 && misoPulldown == 1) {
        out("  -> MISO is held high by a pull-up: the breakout is powered. A card is not proven until CMD0 answers.");
    } else if (misoPullup == 1 && misoPulldown == 0) {
        out("  -> MISO floats: no pull-up on it. The MISO wire or its solder joint is open,");
        out("     the breakout is unpowered, or it has no pull-ups and no card is answering.");
    } else {
        out("  -> MISO stuck LOW: shorted to GND or wrong pin.");
    }

    // Breakouts pull their data lines up to 3.3 V, and the card pulls up CS
    // (DAT3) itself. A line that reads 1 against the chip's pull-down is wired
    // to something that is powered; one that reads 0 reaches nothing.
    out("");
    out("Each line with the chip's pull-down on (1 = held high by the breakout or card):");
    const struct { const char* name; int pin; } lines[] = {
        {"CS  ", SD_CS_PIN}, {"MOSI", SD_MOSI_PIN},
        {"MISO", SD_MISO_PIN}, {"CLK ", SD_CLK_PIN}};
    for (const auto& l : lines) {
        pinMode(l.pin, INPUT_PULLDOWN);
        delay(5);
        outf("  %s GPIO %2d : %d", l.name, l.pin, digitalRead(l.pin));
        pinMode(l.pin, INPUT);
    }
}

// Drives each line low in turn with the other three on pull-ups. A line that
// follows it to 0 is bridged to it, for example by solder between two
// neighbouring header pins.
static void probeShorts() {
    banner("SHORT CHECK");
    const struct { const char* name; int pin; } lines[] = {
        {"CS", SD_CS_PIN}, {"MOSI", SD_MOSI_PIN},
        {"MISO", SD_MISO_PIN}, {"CLK", SD_CLK_PIN}};
    bool any = false;
    for (const auto& drv : lines) {
        for (const auto& l : lines) pinMode(l.pin, INPUT_PULLUP);
        pinMode(drv.pin, OUTPUT);
        digitalWrite(drv.pin, LOW);
        delay(5);
        for (const auto& l : lines) {
            if (l.pin == drv.pin) continue;
            if (digitalRead(l.pin) == 0) {
                outf("  %s (GPIO %d) follows %s (GPIO %d) low: bridged",
                     l.name, l.pin, drv.name, drv.pin);
                any = true;
            }
        }
        pinMode(drv.pin, INPUT);
    }
    for (const auto& l : lines) pinMode(l.pin, INPUT);
    out(any ? "  -> Fix the bridge before anything else." : "  No two lines are bridged.");
}

static void probeRawSD() {
    banner("RAW SD SPI PROBE");

    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);
    sdSPI.begin(SD_CLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    sdSPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));

    // 80 idle clocks with CS high put the card into native SPI mode.
    digitalWrite(SD_CS_PIN, HIGH);
    for (int i = 0; i < 10; ++i) sdSPI.transfer(0xFF);

    digitalWrite(SD_CS_PIN, LOW);
    uint8_t r1 = sdCommand(0, 0x00000000, 0x95);  // CMD0 GO_IDLE_STATE
    outf("CMD0 (GO_IDLE_STATE) response: 0x%02X", r1);
    if (r1 == 0x01) {
        out("  -> Card responded correctly and is in idle state. Wiring is good.");
    } else if (r1 == 0xFF) {
        out("  -> No response at all (MISO silent). Check MISO, CS, CLK, MOSI, 3V3, GND.");
    } else {
        out("  -> Unexpected response; card is talking but not idling as expected.");
    }

    if (r1 == 0x01) {
        uint8_t r8 = sdCommand(8, 0x000001AA, 0x87);  // CMD8 SEND_IF_COND
        uint8_t trailer[4];
        for (int i = 0; i < 4; ++i) trailer[i] = sdSPI.transfer(0xFF);
        outf("CMD8 (SEND_IF_COND) response: 0x%02X  trailer: %02X %02X %02X %02X",
             r8, trailer[0], trailer[1], trailer[2], trailer[3]);
        if (r8 == 0x01 && trailer[3] == 0xAA) {
            out("  -> SDHC/SDXC v2 card, 3.3V compatible.");
        } else if (r8 & 0x04) {
            out("  -> Illegal command: older SDSC v1 card.");
        }
    }

    digitalWrite(SD_CS_PIN, HIGH);
    sdSPI.transfer(0xFF);
    sdSPI.endTransaction();
    sdSPI.end();
}

// --- Raw block read ----------------------------------------------------------
// Runs the whole SPI-mode start-up by hand and reads the CSD register and
// sector 0, printing what comes back. Short command replies can survive a
// marginal line that corrupts a 512-byte data block, so this separates the
// two. It runs at the default output drive strength and again at the
// weakest, which slows the clock edges and reduces ringing on long wires.

// Waits for the start-of-data token (0xFE). Returns the first non-0xFF byte
// seen, or 0xFF if none arrived.
static uint8_t waitToken(uint32_t polls) {
    for (uint32_t i = 0; i < polls; ++i) {
        uint8_t b = sdSPI.transfer(0xFF);
        if (b != 0xFF) return b;
    }
    return 0xFF;
}

// CRC16-CCITT, which SD cards append to every data block.
static uint16_t crc16(const uint8_t* d, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; ++i) {
        c ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; ++b) c = (c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1;
    }
    return c;
}

static uint8_t crc7Byte(uint8_t cmd, uint32_t arg) {
    uint8_t d[5] = {(uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16),
                    (uint8_t)(arg >> 8), (uint8_t)arg};
    uint8_t c = 0;
    for (uint8_t x : d)
        for (int i = 7; i >= 0; --i) {
            uint8_t bit = ((x >> i) & 1) ^ ((c >> 6) & 1);
            c = ((c << 1) & 0x7F) ^ (bit ? 0x09 : 0);
        }
    return (c << 1) | 1;
}

// Reads up to n bytes and returns the first that is not 0xFF.
static uint8_t firstNonFF(int n) {
    for (int i = 0; i < n; ++i) {
        uint8_t b = sdSPI.transfer(0xFF);
        if (b != 0xFF) return b;
    }
    return 0xFF;
}

static void rawBlockTest(gpio_drive_cap_t drive, const char* label) {
    outf("-- %s --", label);
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);
    sdSPI.begin(SD_CLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, -1);
    gpio_set_drive_capability((gpio_num_t)SD_CLK_PIN, drive);
    gpio_set_drive_capability((gpio_num_t)SD_MOSI_PIN, drive);
    gpio_set_drive_capability((gpio_num_t)SD_CS_PIN, drive);
    sdSPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    // A card left mid-block by an earlier failed read keeps sending until it
    // is clocked through the block, and ignores CMD0 until then.
    uint8_t r = 0xFF;
    int resets = 0;
    while (r != 0x01 && resets < 20) {
        digitalWrite(SD_CS_PIN, LOW);
        for (int i = 0; i < 1100; ++i) sdSPI.transfer(0xFF);
        digitalWrite(SD_CS_PIN, HIGH);
        for (int i = 0; i < 10; ++i) sdSPI.transfer(0xFF);
        digitalWrite(SD_CS_PIN, LOW);
        r = sdCommand(0, 0, 0x95);
        ++resets;
    }
    outf("  CMD0  : 0x%02X after %d tries", r, resets);
    if (r == 0x01) {
        r = sdCommand(8, 0x1AA, 0x87);
        uint8_t t[4];
        for (auto& b : t) b = sdSPI.transfer(0xFF);
        outf("  CMD8  : 0x%02X %02X %02X %02X %02X", r, t[0], t[1], t[2], t[3]);

        uint32_t start = millis();
        int tries = 0;
        do {
            sdCommand(55, 0, 0x01);
            r = sdCommand(41, 0x40000000, 0x01);
            ++tries;
        } while (r == 0x01 && millis() - start < 1500);
        outf("  ACMD41: 0x%02X after %d tries, %lu ms (expect 0x00)",
             r, tries, millis() - start);

        bool blockAddr = false;
        if (r == 0x00) {
            r = sdCommand(58, 0, 0x01);
            uint8_t ocr[4];
            for (auto& b : ocr) b = sdSPI.transfer(0xFF);
            blockAddr = ocr[0] & 0x40;
            outf("  CMD58 : 0x%02X OCR %02X %02X %02X %02X (%s addressing)",
                 r, ocr[0], ocr[1], ocr[2], ocr[3], blockAddr ? "block" : "byte");

            r = sdCommand(9, 0, 0x01);
            uint8_t tok = (r == 0x00) ? waitToken(2000) : 0xFF;
            uint8_t csd[18];
            for (auto& b : csd) b = sdSPI.transfer(0xFF);
            uint16_t got = (csd[16] << 8) | csd[17];
            outf("  CMD9  : R1 0x%02X token 0x%02X CRC %s", r, tok,
                 got == crc16(csd, 16) ? "ok" : "BAD");

            // Read sector 0 ten times; every copy must match and pass its CRC.
            static uint8_t blk[514], first[512];
            int ok = 0, crcBad = 0, mismatch = 0, noToken = 0;
            for (int n = 0; n < 10; ++n) {
                r = sdCommand(17, 0, 0x01);
                tok = (r == 0x00) ? waitToken(20000) : 0xFF;
                if (tok != 0xFE) { ++noToken; continue; }
                for (auto& b : blk) b = sdSPI.transfer(0xFF);
                if (((blk[512] << 8) | blk[513]) != crc16(blk, 512)) { ++crcBad; continue; }
                if (ok == 0) memcpy(first, blk, 512);
                else if (memcmp(first, blk, 512) != 0) { ++mismatch; continue; }
                ++ok;
            }
            outf("  CMD17 x10: %d good, %d CRC errors, %d mismatches, %d no token",
                 ok, crcBad, mismatch, noToken);

            // The Arduino driver turns CRC checking on (CMD59). If commands
            // fail only after this, the card sees bad bits on MOSI.
            r = sdCommand(59, 1, 0x83);
            outf("  CMD59 CRC on: 0x%02X (expect 00)", r);
            r = sdCommand(9, 0, 0xAF);  // valid CRC7 for CMD9 arg 0
            tok = (r == 0x00) ? waitToken(2000) : 0xFF;
            for (auto& b : csd) b = sdSPI.transfer(0xFF);
            outf("  CMD9 with CRC on: R1 0x%02X token 0x%02X", r, tok);
            // CS must gate the card: with CS high it has to ignore commands.
            digitalWrite(SD_CS_PIN, HIGH);
            sdSPI.transfer(0xFF);
            uint8_t ignored = sdCommand(13, 0, crc7Byte(13, 0));
            if (ignored == 0xFF) ignored = firstNonFF(16);
            outf("  CMD13 with CS high: 0x%02X (expect FF, card ignores it)", ignored);
            if (ignored != 0xFF) out("  -> Something answered with CS high: CS may not reach the card, or MISO floats without a pull-up.");

            // CMD9 the way the Arduino driver sends it: CS high, then low,
            // wait for MISO to be non-zero, then the command, CRC on.
            digitalWrite(SD_CS_PIN, LOW);
            uint8_t busy = 0;
            for (int i = 0; i < 100; ++i) { busy = sdSPI.transfer(0xFF); if (busy) break; }
            r = sdCommand(9, 0, crc7Byte(9, 0));
            tok = (r == 0x00) ? waitToken(2000) : 0xFF;
            for (auto& b : csd) b = sdSPI.transfer(0xFF);
            outf("  CMD9 after CS toggle: wait 0x%02X, R1 0x%02X token 0x%02X", busy, r, tok);
            sdCommand(59, 0, 0x91);  // CRC off again

            if (ok == 10)
                out("  -> Raw block reads are clean at this drive strength.");
            else
                out("  -> Raw block read FAILED at this drive strength.");
        }
    }
    digitalWrite(SD_CS_PIN, HIGH);
    sdSPI.transfer(0xFF);
    sdSPI.endTransaction();
    sdSPI.end();
    gpio_set_drive_capability((gpio_num_t)SD_CLK_PIN, GPIO_DRIVE_CAP_DEFAULT);
    gpio_set_drive_capability((gpio_num_t)SD_MOSI_PIN, GPIO_DRIVE_CAP_DEFAULT);
    gpio_set_drive_capability((gpio_num_t)SD_CS_PIN, GPIO_DRIVE_CAP_DEFAULT);
    delay(50);
}

// Runs last, because it can leave the card busy until a power cycle: a clean
// raw start-up, then ACMD42 the way the stock driver sends it (CS toggled
// between CMD55 and CMD42), then CMD9.
static void probeAcmd42() {
    banner("ACMD42 CHECK (stock driver step, run last)");
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);
    sdSPI.begin(SD_CLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, -1);
    sdSPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    for (int i = 0; i < 10; ++i) sdSPI.transfer(0xFF);
    digitalWrite(SD_CS_PIN, LOW);
    uint8_t r = sdCommand(0, 0, 0x95);
    sdCommand(8, 0x1AA, 0x87);
    for (int i = 0; i < 4; ++i) sdSPI.transfer(0xFF);
    uint32_t start = millis();
    do {
        sdCommand(55, 0, 0x01);
        r = sdCommand(41, 0x40000000, 0x01);
    } while (r == 0x01 && millis() - start < 1500);
    outf("  start-up ACMD41: 0x%02X", r);
    if (r != 0x00) { digitalWrite(SD_CS_PIN, HIGH); sdSPI.endTransaction(); sdSPI.end(); return; }

    r = sdCommand(9, 0, 0x01);
    uint8_t tok = (r == 0x00) ? waitToken(2000) : 0xFF;
    for (int i = 0; i < 18; ++i) sdSPI.transfer(0xFF);
    outf("  CMD9 before ACMD42: R1 0x%02X token 0x%02X", r, tok);

    uint8_t r55 = sdCommand(55, 0, 0x01);
    digitalWrite(SD_CS_PIN, HIGH);
    digitalWrite(SD_CS_PIN, LOW);
    for (int i = 0; i < 100; ++i) if (sdSPI.transfer(0xFF)) break;
    uint8_t r42 = sdCommand(42, 0, 0x01);
    outf("  ACMD42: CMD55 0x%02X, CMD42 0x%02X", r55, r42);

    r = sdCommand(9, 0, 0x01);
    tok = (r == 0x00) ? waitToken(2000) : 0xFF;
    for (int i = 0; i < 18; ++i) sdSPI.transfer(0xFF);
    outf("  CMD9 after ACMD42: R1 0x%02X token 0x%02X", r, tok);
    out((r == 0x00 && tok == 0xFE)
            ? "  -> ACMD42 is harmless on this card."
            : "  -> ACMD42 breaks this card; the patched driver must not send it.");
    digitalWrite(SD_CS_PIN, HIGH);
    sdSPI.transfer(0xFF);
    sdSPI.endTransaction();
    sdSPI.end();
}

static void probeRawBlocks() {
    banner("RAW BLOCK READ (400 kHz)");
    rawBlockTest(GPIO_DRIVE_CAP_DEFAULT, "default drive strength");
    rawBlockTest(GPIO_DRIVE_CAP_0, "weakest drive strength");
}

// --- Arduino SD driver -------------------------------------------------------

static const char* cardTypeName(uint8_t t) {
    switch (t) {
        case CARD_NONE:    return "NONE";
        case CARD_MMC:     return "MMC";
        case CARD_SD:      return "SDSC";
        case CARD_SDHC:    return "SDHC/SDXC";
        default:           return "UNKNOWN";
    }
}

static void hexdump(const uint8_t* buf, size_t len) {
    char line[80];
    for (size_t i = 0; i < len; i += 16) {
        int n = snprintf(line, sizeof(line), "  %04X: ", (unsigned)i);
        for (size_t j = 0; j < 16 && i + j < len; ++j) {
            n += snprintf(line + n, sizeof(line) - n, "%02X ", buf[i + j]);
        }
        out(String(line));
    }
}

static void listRoot() {
    File root = SD.open("/");
    if (!root) {
        out("Could not open root directory.");
        return;
    }
    out("Root directory:");
    int count = 0;
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        outf("  %-30s %s %u bytes",
             f.name(), f.isDirectory() ? "<DIR>" : "     ", (unsigned)f.size());
        if (++count >= 25) {
            out("  ... (truncated)");
            break;
        }
    }
    if (count == 0) out("  (empty)");
    root.close();
}

// Walks every candidate clock and verifies each one with real reads, rather
// than trusting the first speed that happens to mount. A level-shifted module
// on jumper wires will often enumerate at 20 MHz but corrupt data.
//
// NOTE: this measures the *running* clock, not cold initialisation. The card is
// identified by the 400 kHz pass and stays initialised for the rest of the
// sweep, so a speed passing here is safe to raise the clock to -- but not safe
// to call SD.begin() with on a cold card. See mountSD() in main.cpp.
static uint32_t findBestSpeed() {
    banner("SPI SPEED SWEEP");
    out("Each speed is mounted, then verified by reading sector 0 and comparing");
    out("it with the copy read at the first speed that mounted.");
    out("");
    out("Measures sustained clock only. Card identification always happens at");
    out("400 kHz first, as the SD specification requires.");
    out("");

    const uint32_t freqs[] = {400000, 1000000, 4000000, 8000000,
                              10000000, 16000000, 20000000, 25000000};
    uint8_t reference[512];
    bool haveReference = false;
    uint32_t best = 0;

    for (uint32_t f : freqs) {
        sdSPI.begin(SD_CLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
        if (!SD.begin(SD_CS_PIN, sdSPI, f)) {
            outf("%8u Hz : mount FAILED", f);
            SD.end();
            sdSPI.end();
            delay(100);
            continue;
        }

        uint8_t buf[512];
        bool readOk = SD.readRAW(buf, 0);
        bool match = true;
        if (readOk) {
            if (!haveReference) {
                memcpy(reference, buf, 512);
                haveReference = true;
            } else {
                match = (memcmp(reference, buf, 512) == 0);
            }
        }

        // Time a 64 KB sequential read to get a usable throughput figure.
        uint32_t bytes = 0, t0 = millis();
        for (uint32_t sec = 0; sec < 128; ++sec) {
            if (!SD.readRAW(buf, sec)) { readOk = false; break; }
            bytes += 512;
        }
        uint32_t dt = millis() - t0;
        uint32_t kbps = dt ? (bytes / dt) : 0;

        if (readOk && match) {
            outf("%8u Hz : OK    %u KB/s", f, kbps);
            best = f;
        } else if (!readOk) {
            outf("%8u Hz : read FAILED", f);
        } else {
            outf("%8u Hz : DATA MISMATCH - unreliable", f);
        }

        SD.end();
        sdSPI.end();
        delay(100);
    }

    out("");
    if (best) outf("Highest verified speed: %u Hz", best);
    else      out("No speed produced verified reads.");
    return best;
}

static bool mountAndReport() {
    uint32_t best = findBestSpeed();
    if (!best) return false;

    banner("ARDUINO SD DRIVER");
    sdSPI.begin(SD_CLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    if (!SD.begin(SD_CS_PIN, sdSPI, best)) {
        out("Remount at the chosen speed failed.");
        return false;
    }

    uint8_t type = SD.cardType();
    uint64_t cardSize = SD.cardSize();
    uint32_t secSize = SD.sectorSize();
    uint32_t numSec = SD.numSectors();

    outf("Mount speed   : %u Hz", best);
    outf("Card type     : %s (%u)", cardTypeName(type), type);
    outf("Card size     : %llu bytes (%llu MB)", cardSize, cardSize / (1024ULL * 1024ULL));
    outf("Sector size   : %u bytes", secSize);
    outf("Sector count  : %u", numSec);
    outf("Raw capacity  : %llu bytes", (uint64_t)numSec * secSize);
    outf("FS total      : %llu bytes", SD.totalBytes());
    outf("FS used       : %llu bytes", SD.usedBytes());

    if (secSize == 0 || numSec == 0) {
        out("");
        out("WARNING: sectorSize/numSectors is zero. USB MSC cannot work like this.");
    }

    banner("RAW SECTOR 0 (MBR)");
    uint8_t* sector = (uint8_t*)malloc(secSize ? secSize : 512);
    if (!sector) {
        out("Out of memory for sector buffer.");
        return true;
    }
    if (SD.readRAW(sector, 0)) {
        hexdump(sector, 96);
        out("  ...");
        outf("Boot signature: %02X %02X (expect 55 AA)", sector[510], sector[511]);
        if (sector[510] == 0x55 && sector[511] == 0xAA) {
            const uint8_t* pe = sector + 0x1BE;
            uint32_t lbaStart = pe[8] | (pe[9] << 8) | (pe[10] << 16) | ((uint32_t)pe[11] << 24);
            uint32_t lbaCount = pe[12] | (pe[13] << 8) | (pe[14] << 16) | ((uint32_t)pe[15] << 24);
            outf("Partition 1: type=0x%02X boot=0x%02X startLBA=%u sectors=%u",
                 pe[4], pe[0], lbaStart, lbaCount);
            switch (pe[4]) {
                case 0x0B: case 0x0C: out("  -> FAT32. Good for USB MSC."); break;
                case 0x06: case 0x0E: out("  -> FAT16."); break;
                case 0x07: out("  -> exFAT/NTFS. Windows will read it over MSC, ESP32 will not."); break;
                default: outf("  -> partition type 0x%02X", pe[4]);
            }
        } else {
            out("  -> No MBR signature. Card may be superfloppy-formatted or unreadable.");
        }
    } else {
        out("SD.readRAW(sector 0) FAILED - USB MSC reads would fail too.");
    }
    free(sector);

    banner("FILESYSTEM");
    listRoot();
    return true;
}

void setup() {
    Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
    Serial0.begin(115200);
#endif
    // Send 'A' to also run the ACMD42 check, which leaves this card busy
    // until it is power-cycled.
    bool runAcmd42 = false;
    // Touch the card only once a host is listening: a failed driver mount can
    // leave the card busy until it is power-cycled, so a run nobody reads
    // would cost a replug. Send any byte to start; it starts anyway after 10
    // minutes.
    for (uint32_t t0 = millis(); millis() - t0 < 600000;) {
        if (Serial.available()) {
            while (Serial.available()) runAcmd42 |= (Serial.read() == 'A');
            break;
        }
        if ((millis() - t0) % 5000 < 10) out("Waiting for a byte on the console to start...");
        delay(10);
    }

    banner("ESP32-S3 SD CARD DIAGNOSTIC");
    out("Build: " __DATE__ " " __TIME__);

    reportChip();
    probePins();
    probeShorts();
    probeRawSD();
    probeRawBlocks();
    mountAndReport();
    if (runAcmd42) probeAcmd42();

    banner("DIAGNOSTIC COMPLETE");
}

// After the report, watch the four lines with pull-downs on and print a line
// whenever one changes. Touching a wire's breakout end to 3V3 then shows
// whether that wire reaches the S3.
static int lastLines = -1;

void loop() {
    reflashHatchPoll();
    const int pins[] = {SD_CS_PIN, SD_MOSI_PIN, SD_MISO_PIN, SD_CLK_PIN};
    int state = 0;
    for (int i = 0; i < 4; ++i) {
        pinMode(pins[i], INPUT_PULLDOWN);
        state |= digitalRead(pins[i]) << i;
    }
    if (state != lastLines) {
        outf("[%7lu ms] LIVE CS=%d MOSI=%d MISO=%d CLK=%d", millis(),
             state & 1, (state >> 1) & 1, (state >> 2) & 1, (state >> 3) & 1);
        lastLines = state;
    }
    delay(50);
}
