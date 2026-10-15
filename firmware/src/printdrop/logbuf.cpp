#include "logbuf.h"

#include <esp_attr.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>

namespace logbuf {
namespace {

constexpr size_t   kSize  = 4096;
constexpr uint32_t kMagic = 0x50464c47;  // "PFLG"

RTC_NOINIT_ATTR uint32_t magic;
RTC_NOINIT_ATTR uint32_t written;   // total bytes ever added; position is written % kSize
RTC_NOINIT_ATTR char     ring[kSize];

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

void put(const char* s) {
    portENTER_CRITICAL(&mux);
    for (; *s; ++s) ring[written++ % kSize] = *s;
    ring[written++ % kSize] = '\n';
    portEXIT_CRITICAL(&mux);
}

}  // namespace

const char* resetReason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "external pin";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "other watchdog";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

void begin() {
    // After a power cut RTC memory holds noise; start clean.
    if (magic != kMagic || esp_reset_reason() == ESP_RST_POWERON) {
        magic = kMagic;
        written = 0;
        memset(ring, 0, sizeof(ring));
    }
    char line[64];
    snprintf(line, sizeof(line), "--- boot, reset reason: %s", resetReason());
    put(line);
}

void add(const char* line) {
    if (magic != kMagic) return;
    put(line);
}

String dump() {
    String out;
    portENTER_CRITICAL(&mux);
    const uint32_t n = written;
    portEXIT_CRITICAL(&mux);
    const size_t len = n < kSize ? n : kSize;
    const size_t start = n < kSize ? 0 : n % kSize;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        const char c = ring[(start + i) % kSize];
        if (c) out += c;
    }
    return out;
}

}  // namespace logbuf
