// RuuviTag scanner for ESP32-S3 (Waveshare ESP32-S3-Touch-LCD-2.8)
// Step 1: BLE-scan the RuuviTag (data format 5) and print decoded values over USB serial.
// No display yet — this verifies BLE + decode against the known-good Mac reading.

#include <NimBLEDevice.h>

// RuuviTag manufacturer-specific data: company id 0x0499 (LE bytes 0x99 0x04),
// then payload starting with 0x05 (data format 5 / RAWv2).
static void decodeAndPrint(const std::string& md, const std::string& addr, int rssi) {
  if (md.size() < 26) return;
  const uint8_t* d = reinterpret_cast<const uint8_t*>(md.data());
  if (d[0] != 0x99 || d[1] != 0x04 || d[2] != 0x05) return;  // not a Ruuvi DF5 frame

  int16_t  tRaw   = (int16_t)((d[3] << 8) | d[4]);
  float    tempC  = tRaw * 0.005f;
  uint16_t hRaw   = (d[5] << 8) | d[6];
  float    hum    = hRaw * 0.0025f;
  uint16_t pRaw   = (d[7] << 8) | d[8];
  float    hpa    = (pRaw + 50000) / 100.0f;
  // power info at payload offset 13-14 -> raw md index 15-16 (company id occupies 0-1)
  uint16_t battRaw = (d[15] << 3) | (d[16] >> 5);
  uint16_t mv      = battRaw + 1600;

  Serial.printf("RuuviTag %s  RSSI %d dBm\n", addr.c_str(), rssi);
  Serial.printf("  Temp %.2f C   Hum %.2f %%   Pres %.2f hPa   Batt %u mV\n",
                tempC, hum, hpa, mv);
}

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    std::string md = dev->getManufacturerData();
    if (!md.empty()) {
      decodeAndPrint(md, dev->getAddress().toString(), dev->getRSSI());
    }
  }
};

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n[RuuviBeacon] BLE scanner starting...");

  NimBLEDevice::init("");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(new ScanCallbacks());
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->start(0);  // scan continuously
  Serial.println("[RuuviBeacon] Scanning for RuuviTag (DF5)...");
}

void loop() {
  delay(1000);
}
