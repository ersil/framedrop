#pragma once
#include <Arduino.h>

// The board has no serial console once USB is the drive, so log lines also go
// into a ring buffer that /api/log serves. It lives in RTC memory, which
// survives a crash, watchdog or software reset (not a power cut), so the lines
// before an unexpected reboot can still be read afterwards.
namespace logbuf {
void begin();
void add(const char* line);
String dump();
const char* resetReason();
}
