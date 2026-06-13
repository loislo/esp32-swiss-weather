// RuuviBeacon — Waveshare ESP32-S3-Touch-LCD-2.8 (ST7789 240x320, CST328 touch, PCM5101 audio)
// Step 2: BLE-scan the RuuviTag (DF5) and show the temperature on the LCD.
// Pins are from Waveshare's official demo (Display_ST7789.h). Crisp text via U8g2 fonts.

#include <NimBLEDevice.h>
#include <Arduino_GFX_Library.h>
#include <U8g2lib.h>   // for u8g2_font_* symbols rendered by Arduino_GFX
#include <ESP_I2S.h>   // PCM5101 I2S DAC for the chime
#include <math.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Preferences.h>   // NVS key-value storage for runtime config

// ---- LCD (ST7789) pins, from Waveshare demo ----
#define PIN_MOSI 45
#define PIN_SCLK 40
#define PIN_LCD_CS 42
#define PIN_LCD_DC 41
#define PIN_LCD_RST 39
#define PIN_LCD_BL 5

Arduino_DataBus* bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_SCLK, PIN_MOSI, -1);
Arduino_GFX* output = new Arduino_ST7789(bus, PIN_LCD_RST, 1 /*landscape 320x240*/, true /*IPS*/,
                                         240, 320, 0, 0, 0, 0);
// double-buffered canvas (framebuffer in PSRAM) -> flicker-free updates
Arduino_Canvas* gfx = new Arduino_Canvas(320, 240, output);

// ---- latest reading shared from BLE callback to draw loop ----
volatile bool   haveReading = false;
volatile float  gTempC = 0, gHum = 0, gHpa = 0;
volatile uint16_t gMv = 0;
volatile int    gRssi = 0;
volatile uint32_t gSeq = 0;   // increments each new frame, triggers redraw

// ---- runtime config (persisted in NVS, set over serial; no secrets in source) ----
Preferences prefs;
String cfgWifi, cfgPass;
int    cfgPlz = 0;        // postal-code override (0 = auto from IP)

// ---- WiFi / IP-geolocation / time / outdoor weather ----
char  gCity[32] = "";
int   gPlz      = 0;     // postal code, auto-detected from IP
long  gOffset   = 0;     // UTC offset in seconds, auto-detected from IP
bool  gGeoDone  = false;
volatile float gTdMin = 0, gTdMax = 0, gTmMin = 0, gTmMax = 0;  // today/tomorrow min-max
volatile bool  gHaveWx = false;

static void decode(const std::string& md, int rssi) {
  if (md.size() < 26) return;
  const uint8_t* d = reinterpret_cast<const uint8_t*>(md.data());
  if (d[0] != 0x99 || d[1] != 0x04 || d[2] != 0x05) return;  // not Ruuvi DF5
  gTempC = (int16_t)((d[3] << 8) | d[4]) * 0.005f;
  gHum   = ((d[5] << 8) | d[6]) * 0.0025f;
  gHpa   = (((d[7] << 8) | d[8]) + 50000) / 100.0f;
  gMv    = (((d[15] << 3) | (d[16] >> 5))) + 1600;
  gRssi  = rssi;
  haveReading = true;
  gSeq++;
}

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    std::string md = dev->getManufacturerData();
    if (!md.empty()) decode(md, dev->getRSSI());
  }
};

// width of a string in the current font
static uint16_t textW(const char* s) {
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}
// print a string horizontally centered on cx, at the given baseline y
static void printCentered(const char* s, int16_t cx, int16_t baselineY) {
  gfx->setCursor(cx - textW(s) / 2, baselineY);
  gfx->print(s);
}

// ---- audio: PCM5101 I2S DAC (DOUT 47, BCLK 48, LRC 38), no enable/mute pin ----
#define I2S_DOUT 47
#define I2S_BCLK 48
#define I2S_LRC  38
I2SClass i2s;
static const uint32_t AUDIO_SR = 16000;

static void playTone(float freq, int ms, float vol) {
  const int total = (int)((long)AUDIO_SR * ms / 1000);
  int16_t chunk[256 * 2];
  int idx = 0;
  for (int n = 0; n < total; n++) {
    float env = sinf((float)M_PI * n / total);   // raised-sine envelope (no clicks)
    int16_t s = (int16_t)(vol * env * 11000.0f * sinf(2.0f * (float)M_PI * freq * n / AUDIO_SR));
    chunk[idx++] = s;  // left
    chunk[idx++] = s;  // right
    if (idx >= (int)(sizeof(chunk) / sizeof(chunk[0]))) {
      i2s.write((uint8_t*)chunk, idx * sizeof(int16_t));
      idx = 0;
    }
  }
  if (idx) i2s.write((uint8_t*)chunk, idx * sizeof(int16_t));
}

static void playChime() {
  playTone(880.0f, 90, 0.6f);     // A5
  playTone(1318.5f, 150, 0.6f);   // E6  -> a rising "di-ding"
}

// ---- temperature -> color: freezing blue ... teal ... yellow ... red ----
static uint16_t tempColor(float t) {
  struct Stop { float t; uint8_t r, g, b; };
  static const Stop st[] = {
    { -5,  40, 120, 255 },   // icy blue
    {  0,   0, 170, 255 },   // blue
    { 10,   0, 230, 180 },   // teal
    { 18, 170, 230,  60 },   // lime
    { 25, 255, 220,   0 },   // yellow
    { 31, 255, 140,   0 },   // orange
    { 36, 255,  40,   0 },   // red
  };
  const int n = sizeof(st) / sizeof(st[0]);
  if (t <= st[0].t)     return gfx->color565(st[0].r, st[0].g, st[0].b);
  if (t >= st[n - 1].t) return gfx->color565(st[n - 1].r, st[n - 1].g, st[n - 1].b);
  for (int i = 0; i < n - 1; i++) {
    if (t < st[i + 1].t) {
      float f = (t - st[i].t) / (st[i + 1].t - st[i].t);
      return gfx->color565(st[i].r + f * (st[i + 1].r - st[i].r),
                           st[i].g + f * (st[i + 1].g - st[i].g),
                           st[i].b + f * (st[i + 1].b - st[i].b));
    }
  }
  return RGB565_WHITE;
}

// ---- IP geolocation: auto-detect postal code + UTC offset, set the clock ----
static void geolocate() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, "http://ip-api.com/json/?fields=status,city,zip,offset")) return;
  if (http.GET() == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString()) && doc["status"] == "success") {
      gOffset = doc["offset"] | 0L;
      gPlz    = atoi(doc["zip"] | "0");
      snprintf(gCity, sizeof(gCity), "%s", doc["city"] | "");
      if (gPlz > 0) {
        gGeoDone = true;
        configTime(gOffset, 0, "pool.ntp.org", "time.google.com");  // clock from offset
        Serial.printf("[geo] %s  plz=%d  offset=%lds\n", gCity, gPlz, gOffset);
      }
    }
  }
  http.end();
}

// ---- MeteoSwiss: today's max temperature for the detected postal code ----
static void fetchMeteo() {
  int plz = (cfgPlz > 0) ? cfgPlz : gPlz;   // explicit override wins over IP geolocation
  if (WiFi.status() != WL_CONNECTED || plz <= 0) return;
  char url[96];
  snprintf(url, sizeof(url), "https://app-prod-ws.meteoswiss-app.ch/v1/forecast?plz=%d00", plz);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  if (!https.begin(client, url)) return;
  https.addHeader("User-Agent", "Mozilla/5.0");
  int code = https.GET();
  if (code == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, https.getString())) {
      JsonArray arr = doc["regionForecast"].as<JsonArray>();
      if (!arr.isNull() && arr.size() >= 2) {
        gTdMin = arr[0]["temperatureMin"] | 0.0f;
        gTdMax = arr[0]["temperatureMax"] | 0.0f;
        gTmMin = arr[1]["temperatureMin"] | 0.0f;
        gTmMax = arr[1]["temperatureMax"] | 0.0f;
        gHaveWx = true;
        Serial.printf("[meteo] today %.0f-%.0f  tomorrow %.0f-%.0f\n",
                      gTdMin, gTdMax, gTmMin, gTmMax);
      }
    }
  } else {
    Serial.printf("[meteo] HTTP %d\n", code);
  }
  https.end();
}

// ---- capacitive touch (CST328) on its own I2C bus: SDA 1, SCL 3, INT 4, RST 2 ----
#define TP_SDA 1
#define TP_SCL 3
#define TP_INT 4
#define TP_RST 2
#define TP_ADDR 0x1A

static bool tpRead(uint16_t reg, uint8_t* data, size_t len) {
  Wire1.beginTransmission(TP_ADDR);
  Wire1.write((uint8_t)(reg >> 8));
  Wire1.write((uint8_t)reg);
  if (Wire1.endTransmission(true)) return false;
  Wire1.requestFrom((uint8_t)TP_ADDR, (uint8_t)len);
  for (size_t i = 0; i < len && Wire1.available(); i++) data[i] = Wire1.read();
  return true;
}
static void tpWrite(uint16_t reg, const uint8_t* data, size_t len) {
  Wire1.beginTransmission(TP_ADDR);
  Wire1.write((uint8_t)(reg >> 8));
  Wire1.write((uint8_t)reg);
  for (size_t i = 0; i < len; i++) Wire1.write(data[i]);
  Wire1.endTransmission(true);
}
static void touchInit() {
  Wire1.begin(TP_SDA, TP_SCL, 400000);
  pinMode(TP_INT, INPUT);
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, HIGH); delay(50);
  digitalWrite(TP_RST, LOW);  delay(5);
  digitalWrite(TP_RST, HIGH); delay(50);
  uint8_t b = 0;
  tpWrite(0xD109, &b, 0);   // CST328 -> normal mode
}
// returns true if a finger is present; fills raw panel-native x,y
static bool touchRead(uint16_t* rx, uint16_t* ry) {
  uint8_t n = 0, clear = 0;
  if (!tpRead(0xD005, &n, 1)) return false;
  uint8_t cnt = n & 0x0F;
  if (cnt == 0 || cnt > 5) { tpWrite(0xD005, &clear, 1); return false; }
  uint8_t buf[27];
  tpRead(0xD000, buf, 27);
  tpWrite(0xD005, &clear, 1);
  *rx = ((uint16_t)buf[1] << 4) + ((buf[3] & 0xF0) >> 4);
  *ry = ((uint16_t)buf[2] << 4) + (buf[3] & 0x0F);
  return true;
}

// RuuviTag coin cell (CR2477) is low around ~2.5 V.
#define LOW_BATT_MV 2500

static void drawScreen() {
  const int16_t W = gfx->width();    // 320 (landscape)
  gfx->fillScreen(RGB565_BLACK);

  // ---- top bar: clock (left) + outdoor daily max (right) ----
  gfx->setFont(u8g2_font_helvB18_tf);
  char clk[6] = "--:--";
  struct tm ti;
  if (getLocalTime(&ti, 5)) strftime(clk, sizeof(clk), "%H:%M", &ti);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(6, 24);
  gfx->print(clk);
  if (gHaveWx) {
    char tm[24];
    snprintf(tm, sizeof(tm), "Tomorrow %.0f-%.0f°", gTmMin, gTmMax);   // tomorrow min-max
    gfx->setFont(u8g2_font_helvB12_tf);
    gfx->setTextColor(tempColor(gTmMax));
    gfx->setCursor(W - 6 - textW(tm), 22);
    gfx->print(tm);
  }

  if (!haveReading) {
    gfx->setFont(u8g2_font_fur30_tr);
    gfx->setTextColor(RGB565_YELLOW);
    printCentered("...", W / 2, 130);
  } else {
    // ---- big indoor temperature: "17.1°C" (Free Universal, full-glyph) ----
    char temp[24];
    snprintf(temp, sizeof(temp), "%.1f°C", gTempC);
    gfx->setFont(u8g2_font_fur42_tf);
    gfx->setTextColor(tempColor(gTempC));   // warm = red, cold = blue
    printCentered(temp, W / 2, 128);

    // ---- today's outdoor min-max, smaller, just below the big number ----
    if (gHaveWx) {
      char td[24];
      snprintf(td, sizeof(td), "Today %.0f-%.0f°", gTdMin, gTdMax);
      gfx->setFont(u8g2_font_helvB14_tf);
      gfx->setTextColor(gfx->color565(180, 180, 180));
      printCentered(td, W / 2, 164);
    }

    // ---- one stats line: humidity, pressure, voltage, signal ----
    gfx->setFont(u8g2_font_helvR14_tr);
    gfx->setTextColor(gfx->color565(180, 180, 180));
    char line[64];
    snprintf(line, sizeof(line), "%.0f%%  %.0fmmHg  %.1fV  %ddBm",
             gHum, gHpa * 0.750062f, gMv / 1000.0f, gRssi);
    printCentered(line, W / 2, 233);
  }

  // ---- warning line (second from the bottom): low battery / no WiFi ----
  const char* warn = nullptr;
  uint16_t    warnColor = 0;
  if (haveReading && gMv > 0 && gMv < LOW_BATT_MV) {
    warn = "Beacon battery low";
    warnColor = RGB565_RED;
  } else if (WiFi.status() != WL_CONNECTED && (cfgWifi.length() == 0 || millis() > 20000)) {
    warn = cfgWifi.length() ? "WiFi failed - check setup over USB"
                            : "No WiFi - set over USB: type help";
    warnColor = gfx->color565(255, 170, 0);   // amber
  }
  if (warn) {
    gfx->setFont(u8g2_font_helvB14_tr);
    gfx->setTextColor(warnColor);
    printCentered(warn, W / 2, 208);
  }

  gfx->flush();   // push framebuffer to the LCD in one shot -> no flicker
}

// ======================= serial config console =======================
static void applyWifi() {
  WiFi.disconnect();
  gGeoDone = false; gHaveWx = false;          // re-detect location + weather
  if (cfgWifi.length()) {
    WiFi.begin(cfgWifi.c_str(), cfgPass.c_str());
    Serial.printf("[cfg] reconnecting WiFi to '%s'...\n", cfgWifi.c_str());
  }
}

static void printHelp() {
  Serial.println(F("\n=== RuuviBeacon config console ==="));
  Serial.println(F("  help            this help"));
  Serial.println(F("  show            current values"));
  Serial.println(F("  <key>=<value>   set a parameter (saved to flash, used after reboot too)"));
  Serial.println(F("keys:"));
  Serial.println(F("  wifi            WiFi SSID"));
  Serial.println(F("  wifi_pass       WiFi password"));
  Serial.println(F("  plz             postal-code override for weather (0 = auto from IP)"));
  Serial.println(F("examples:  wifi=YourSSID   |   wifi_pass=secret123   |   plz=8802\n"));
}

static void printShow() {
  Serial.println(F("\n=== current ==="));
  if (haveReading)
    Serial.printf("temp      = %.2f C  (hum %.1f%%, %.1f hPa, %u mV, %d dBm)\n",
                  gTempC, gHum, gHpa, gMv, gRssi);
  else
    Serial.println(F("temp      = (no beacon reading yet)"));
  Serial.printf("wifi      = %s\n", cfgWifi.length() ? cfgWifi.c_str() : "(unset)");
  Serial.printf("wifi_pass = %s\n", cfgPass.length() ? "******** (set)" : "(unset)");
  Serial.printf("plz       = %d %s\n", cfgPlz, cfgPlz > 0 ? "(override)" : "(auto)");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("wifi state= connected, IP "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("wifi state= not connected"));
  }
  if (gGeoDone) Serial.printf("location  = %s (plz %d), UTC offset %lds\n", gCity, gPlz, gOffset);
  if (gHaveWx)  Serial.printf("weather   = today %.0f-%.0f, tomorrow %.0f-%.0f\n",
                              gTdMin, gTdMax, gTmMin, gTmMax);
  Serial.println();
}

static void setConfig(String key, String val) {
  key.trim(); val.trim();
  if (key == "wifi") {
    cfgWifi = val; prefs.putString("wifi", val);
    Serial.printf("[cfg] wifi = %s (saved)\n", val.c_str());
    applyWifi();
  } else if (key == "wifi_pass") {
    cfgPass = val; prefs.putString("wifi_pass", val);
    Serial.println(F("[cfg] wifi_pass saved"));
    applyWifi();
  } else if (key == "plz") {
    cfgPlz = val.toInt(); prefs.putInt("plz", cfgPlz);
    gHaveWx = false;                      // force weather refresh
    Serial.printf("[cfg] plz = %d (saved)\n", cfgPlz);
  } else {
    Serial.printf("[cfg] unknown key '%s' - type 'help'\n", key.c_str());
  }
}

static void handleLine(char* line) {
  String s(line); s.trim();
  if (!s.length()) return;
  if (s.equalsIgnoreCase("help") || s == "?")    { printHelp(); return; }
  if (s.equalsIgnoreCase("show") ||
      s.equalsIgnoreCase("status")) { printShow(); return; }
  int eq = s.indexOf('=');
  if (eq > 0) { setConfig(s.substring(0, eq), s.substring(eq + 1)); return; }
  Serial.println(F("unknown command - type 'help'"));
}

static void pollSerial() {
  static char buf[128];
  static size_t len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (len) { buf[len] = 0; handleLine(buf); len = 0; }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
}
// =====================================================================

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n[RuuviBeacon] starting (display build)...");

  ledcAttach(PIN_LCD_BL, 5000, 8);  // backlight PWM: 5 kHz, 8-bit (0-255)
  ledcWrite(PIN_LCD_BL, 255);       // full brightness
  touchInit();
  gfx->begin(80000000);
  gfx->setUTF8Print(true);
  Serial.printf("[RuuviBeacon] free PSRAM %u, heap %u\n", ESP.getFreePsram(), ESP.getFreeHeap());
  drawScreen();                     // initial "scanning..." screen

  // I2S audio (PCM5101)
  i2s.setPins(I2S_BCLK, I2S_LRC, I2S_DOUT);
  if (!i2s.begin(I2S_MODE_STD, AUDIO_SR, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("[RuuviBeacon] I2S init failed");
  }

  // load saved config from NVS (WiFi creds, plz)
  prefs.begin("cfg", false);
  cfgWifi = prefs.getString("wifi", "");
  cfgPass = prefs.getString("wifi_pass", "");
  cfgPlz  = prefs.getInt("plz", 0);

  WiFi.mode(WIFI_STA);
  if (cfgWifi.length()) {
    WiFi.begin(cfgWifi.c_str(), cfgPass.c_str());
    Serial.printf("[RuuviBeacon] WiFi connecting to '%s'...\n", cfgWifi.c_str());
  } else {
    Serial.println(F("[RuuviBeacon] No WiFi set. Type 'help', e.g.  wifi=YourSSID  then  wifi_pass=..."));
  }
  Serial.println(F("[RuuviBeacon] Type 'help' for config commands."));

  NimBLEDevice::init("");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(new ScanCallbacks());
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->start(0);
  Serial.println("[RuuviBeacon] scanning for RuuviTag...");
}

void loop() {
  static uint32_t lastDrawn = 0xFFFFFFFF;
  static uint32_t lastClock = 0;
  static uint32_t lastGeo   = 0;
  static uint32_t lastMeteo = 0;
  static uint32_t lastTempPrint = 0;

  // --- serial config console + occasional live temperature line ---
  pollSerial();
  if (haveReading && millis() - lastTempPrint > 10000) {
    lastTempPrint = millis();
    Serial.printf("temp %.2f C  hum %.1f%%  %u mV  %d dBm\n", gTempC, gHum, gMv, gRssi);
  }

  // --- background network: geolocate once, then refresh outdoor max ---
  if (WiFi.status() == WL_CONNECTED) {
    if (!gGeoDone && (lastGeo == 0 || millis() - lastGeo > 30000)) {
      lastGeo = millis();
      geolocate();
    }
    if (gGeoDone || cfgPlz > 0) {
      uint32_t interval = gHaveWx ? 1800000UL : 30000UL;  // 30 min; retry every 30 s
      if (lastMeteo == 0 || millis() - lastMeteo > interval) {
        lastMeteo = millis();
        fetchMeteo();
      }
    }
  }

  // --- touch: vertical finger position controls backlight brightness ---
  static uint32_t lastTouch = 0;
  if (millis() - lastTouch > 40) {
    lastTouch = millis();
    uint16_t tx, ty;
    if (touchRead(&tx, &ty)) {
      int v = constrain((int)tx, 0, 240);          // screen-vertical = raw X (landscape)
      int bright = map(v, 0, 240, 15, 255);        // finger down = dim, up = bright
      ledcWrite(PIN_LCD_BL, bright);
    }
  }

  // --- beacon presence: chime once when we (re)acquire the beacon ---
  static uint32_t lastReadingMs = 0;
  static bool     beaconPresent = false;
  bool     newReading = (gSeq != lastDrawn);
  uint32_t nowMs = millis();
  if (newReading) {
    if (!beaconPresent) {           // absent -> present == we got the beacon
      beaconPresent = true;
      playChime();
    }
    lastReadingMs = nowMs;
  } else if (beaconPresent && nowMs - lastReadingMs > 10000) {
    beaconPresent = false;          // no reading for 10 s -> consider it lost
  }

  // --- redraw on a new reading, or once a second so the clock ticks ---
  if (newReading || nowMs - lastClock > 1000) {
    lastClock = nowMs;
    lastDrawn = gSeq;
    drawScreen();
  }
  delay(100);
}
