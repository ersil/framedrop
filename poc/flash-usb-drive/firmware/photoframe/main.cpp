#include <Arduino.h>

#include "USB.h"
#include "USBMSC.h"
#include "esp_partition.h"

// Gate 1: present the "photos" flash partition to the USB host as a
// read-only mass-storage drive. The partition holds a FAT image built on the
// Mac (tools/make_disk.sh) and flashed at 0x1F0000, so it survives reboots.

namespace {

constexpr uint16_t kBlockSize = 512;

USBMSC msc;
const esp_partition_t *disk = nullptr;

int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  const size_t address = static_cast<size_t>(lba) * kBlockSize + offset;
  if (address + bufsize > disk->size) {
    return -1;
  }
  if (esp_partition_read(disk, address, buffer, bufsize) != ESP_OK) {
    return -1;
  }
  return bufsize;
}

int32_t onWrite(uint32_t lba, uint32_t, uint8_t *, uint32_t) {
  Serial.printf("Rejected host write at LBA %lu\n", static_cast<unsigned long>(lba));
  return -1;
}

bool onStartStop(uint8_t powerCondition, bool start, bool loadEject) {
  Serial.printf("MSC start/stop: power=%u start=%u eject=%u\n", powerCondition,
                start, loadEject);
  return true;
}

}  // namespace

void setup() {
  Serial.begin(115200);

  disk = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                  ESP_PARTITION_SUBTYPE_ANY, "photos");
  if (disk == nullptr) {
    // Without the partition there is nothing to serve; keep USB CDC up so
    // the error is visible on the serial console.
    USB.begin();
    while (true) {
      Serial.println("photos partition not found; check partitions.csv");
      delay(2000);
    }
  }

  msc.vendorID("ESP32");
  msc.productID("PhotoFrame");
  msc.productRevision("0.1");
  msc.onRead(onRead);
  msc.onWrite(onWrite);
  msc.onStartStop(onStartStop);
  msc.isWritable(false);
  msc.mediaPresent(true);
  msc.begin(disk->size / kBlockSize, kBlockSize);
  USB.begin();
}

void loop() {
  static uint32_t lastReport = 0;
  if (millis() - lastReport >= 10000) {
    lastReport = millis();
    Serial.printf("Serving %lu sectors from partition at 0x%lx\n",
                  static_cast<unsigned long>(disk->size / kBlockSize),
                  static_cast<unsigned long>(disk->address));
  }
  delay(10);
}
