#pragma once
#include <Arduino.h>

// OTA via HTTP POST /api/ota (Update) and via SD file detection.
// Call ota::begin() after storage::begin() (needs SD FS). Poll ota::loop() or
// call ota::checkSD() periodically to detect firmware.bin on SD.
namespace ota {

void begin();
void loop();

// SD OTA detection: returns true if firmware.bin + firmware.json with newer version found.
// Caller can show popup and call triggerSDUpdate().
bool checkSD(String* outVersion = nullptr, String* outNotes = nullptr);
bool triggerSDUpdate(String* err = nullptr);

// HTTP upload handler helpers
// uiImage: write the LittleFS web UI image instead of the firmware.
bool beginUpdate(size_t totalSize, String* err, bool uiImage = false);
bool writeUpdate(uint8_t* data, size_t len, String* err);
bool endUpdate(String* err);
bool abortUpdate();

bool isUpdating();
String currentVersion();

// Marks the running image valid if it is still on probation after an OTA, so
// the bootloader does not roll it back. Called once it has run 60 s, and
// before any restart the firmware chooses: an image that is up and choosing to
// restart has proven itself, and rolling back on, say, the restart after a web
// UI update would undo a good firmware update.
void markRunningValid();

}  // namespace ota
