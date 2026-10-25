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

// Declared up here because the Arduino preprocessor hoists its generated
// prototypes above the body of the sketch, and they reference this type.
enum Cond { COND_CLEAR, COND_FAIR, COND_PARTLY, COND_CLOUDY, COND_OVERCAST,
            COND_FOG, COND_SHOWERS, COND_RAIN, COND_SLEET, COND_SNOW, COND_THUNDER };

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
bool  gHaveTmr = false;   // forecast[1] present, so tomorrow can be shown

// ---- what the weather screen draws beyond today's min-max ----
float gOutTemp  = 0;     // currentWeather.temperature -- the hero number
int   gOutIcon  = 0;     // currentWeather.iconV2      -- drives condition + pictogram
int   gRainProb = -1;    // precipitation probability for the current 3 h slot, %
int   gRainTd   = -1;    // peak probability today, %
int   gRainTm   = -1;    // ... and tomorrow
#define HOURS 9              // columns in the hourly strip
struct HourCol { char label[3]; float temp; int pop; };
HourCol gHours[HOURS];
// Cached forecast series. The window the strip shows is slid onto these by
// rebuildHours() using the current clock, so it follows the hour instead of
// only moving when a fetch happens.
float   gT1h[168];
uint8_t gP3h[48];
int     gT1hN = 0, gP3hN = 0;
time_t  gGraphStart = 0;
bool  gHaveHours = false;

// beacon liveness: set by the BLE callback, read by the draw loop
volatile uint32_t gLastMs = 0;
String cfgBeacon;        // label for the beacon strip (design shows "Balcony")
String cfgCity;          // header label; overrides the IP-derived city name

static void decode(const std::string& md, int rssi) {
  if (md.size() < 26) return;
  const uint8_t* d = reinterpret_cast<const uint8_t*>(md.data());
  if (d[0] != 0x99 || d[1] != 0x04 || d[2] != 0x05) return;  // not Ruuvi DF5
  gTempC = (int16_t)((d[3] << 8) | d[4]) * 0.005f;
  gHum   = ((d[5] << 8) | d[6]) * 0.0025f;
  gHpa   = (((d[7] << 8) | d[8]) + 50000) / 100.0f;
  gMv    = (((d[15] << 3) | (d[16] >> 5))) + 1600;
  gRssi  = rssi;
  gLastMs = millis();
  haveReading = true;
  gSeq++;
}

// --- BLE scan scheduling -------------------------------------------------
// WiFi and BLE share one 2.4 GHz radio, so a continuous scan starves the WiFi
// link however low its duty cycle. Scan in short bursts instead and leave the
// radio alone in between: a RuuviTag advertises about once a second, so a few
// seconds catches one, and the burst ends the moment a reading lands. A fresh
// reading buys a full minute of quiet; with nothing heard we retry sooner but
// still hand most of the airtime back.
#define SCAN_BURST_MS 3000
#define SCAN_FRESH_MS 60000
#define SCAN_RETRY_MS 15000
volatile bool     gScanActive = false;
volatile uint32_t gScanEnded  = 0;

volatile uint32_t gAdvSeen = 0;      // every advertisement the radio reports
volatile uint32_t gAdvMfg  = 0;      // ... of which carried manufacturer data
bool gBleDebug = false;              // "ble=1" over the console dumps them

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    gAdvSeen++;
    std::string md = dev->getManufacturerData();
    if (!md.empty()) {
      gAdvMfg++;
      if (gBleDebug) {
        char hex[40] = {0};
        size_t n = md.size() < 12 ? md.size() : 12;
        for (size_t i = 0; i < n; i++) snprintf(hex + i * 3, 4, "%02X ", (uint8_t)md[i]);
        Serial.printf("[ble] %s rssi=%d len=%u mfg=%s\n",
                      dev->getAddress().toString().c_str(), dev->getRSSI(),
                      (unsigned)md.size(), hex);
      }
      decode(md, dev->getRSSI());
    }
  }
  void onScanEnd(const NimBLEScanResults&, int) override {
    gScanActive = false;
    gScanEnded  = millis();          // loop() schedules the next burst
  }
};

// Arduino_GFX's getTextBounds reports the INK box, not the pen advance, and
// u8g2 glyphs carry a per-glyph x offset -- logisoso's "1" starts several
// pixels right of the pen. Measuring with the ink width alone therefore puts
// the next element too far left and the two overlap, so carry the left
// bearing around with the width.
static uint16_t textExt(const char* s, int16_t* lead) {
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  if (lead) *lead = x1;
  return w;
}
static uint16_t textW(const char* s) { return textExt(s, nullptr); }
// distance from the pen to just past the ink -- what to advance by
static int16_t textAdv(const char* s) {
  int16_t lead; uint16_t w = textExt(s, &lead);
  return lead + (int16_t)w;
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
        // The clock is NOT set from this offset -- see setup(). gOffset is kept
        // only as a sanity check against the timezone rule in the serial log.
        Serial.printf("[geo] %s  plz=%d  ip-api offset=%lds\n", gCity, gPlz, gOffset);
      }
    }
  }
  http.end();
}

// ---- MeteoSwiss: current conditions, today's min-max, and a 6 h outlook ----
// This uses /v1/plzDetail rather than /v1/forecast: the screen needs
// precipitationProbability3h (the hero's "RAIN n%" and the hourly bars) and
// that array only exists on plzDetail. graph.start is local midnight expressed
// as an absolute epoch, and the 1 h samples run from there, so the sample for
// a given moment is just (now - start) / 1 h, and its 3 h probability slot is
// that index / 3.
static void fetchMeteo() {
  int plz = (cfgPlz > 0) ? cfgPlz : gPlz;   // explicit override wins over IP geolocation
  if (WiFi.status() != WL_CONNECTED || plz <= 0) return;
  char url[96];
  snprintf(url, sizeof(url), "https://app-prod-ws.meteoswiss-app.ch/v1/plzDetail?plz=%d00", plz);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  if (!https.begin(client, url)) return;
  https.addHeader("User-Agent", "Mozilla/5.0");
  int code = https.GET();
  if (code == 200) {
    // The payload carries ~20 arrays of 144 floats; we draw six of them. Filter
    // so only the fields below are ever built into the document, and stream it
    // straight from the socket instead of holding the body as a String too.
    JsonDocument filter;
    filter["currentWeather"]["temperature"]       = true;
    filter["currentWeather"]["iconV2"]            = true;
    filter["forecast"][0]["temperatureMax"]       = true;
    filter["forecast"][0]["temperatureMin"]       = true;
    filter["graph"]["start"]                      = true;
    filter["graph"]["temperatureMean1h"]          = true;
    filter["graph"]["precipitationProbability3h"] = true;

    JsonDocument doc;
    // Buffer the body rather than parsing straight off https.getStream():
    // streaming with a Filter silently yields an EMPTY document on this stack --
    // deserializeJson() still reports Ok, but every field reads back zero. Cost
    // is ~15 kB of String against ~280 kB of free heap, and the filter still
    // keeps the resulting DOM to the handful of fields we draw.
    String body = https.getString();
    DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err) Serial.printf("[meteo] parse failed after %u bytes: %s\n",
                           (unsigned)body.length(), err.c_str());
    if (!err) {
      gOutTemp = doc["currentWeather"]["temperature"] | 0.0f;
      gOutIcon = doc["currentWeather"]["iconV2"] | 0;

      JsonArray fc = doc["forecast"].as<JsonArray>();
      if (!fc.isNull() && fc.size() >= 1) {
        gTdMin = fc[0]["temperatureMin"] | 0.0f;
        gTdMax = fc[0]["temperatureMax"] | 0.0f;
        gHaveTmr = (fc.size() >= 2);
        if (gHaveTmr) {
          gTmMin = fc[1]["temperatureMin"] | 0.0f;
          gTmMax = fc[1]["temperatureMax"] | 0.0f;
        }
        gHaveWx = true;
      }

      long long startMs = doc["graph"]["start"] | 0LL;
      JsonArray t1h     = doc["graph"]["temperatureMean1h"].as<JsonArray>();
      JsonArray p3h     = doc["graph"]["precipitationProbability3h"].as<JsonArray>();

      // Cache the whole series. None of this needs a clock, so a fetch that
      // lands before NTP has synced is no longer wasted -- drawScreen() picks
      // the window as soon as the time is good.
      gGraphStart = (time_t)(startMs / 1000);
      gT1hN = gP3hN = 0;
      if (!t1h.isNull())
        for (JsonVariant v : t1h) {
          if (gT1hN >= (int)(sizeof(gT1h) / sizeof(gT1h[0]))) break;
          gT1h[gT1hN++] = v | 0.0f;
        }
      if (!p3h.isNull())
        for (JsonVariant v : p3h) {
          if (gP3hN >= (int)sizeof(gP3h)) break;
          int x = (int)lroundf(v | 0.0f);          // floats in the JSON, see above
          gP3h[gP3hN++] = (uint8_t)(x < 0 ? 0 : (x > 100 ? 100 : x));
        }

      // Daily peak rain chance. graph.start is local midnight, so the 3 h slots
      // land on day boundaries -- 0..7 is today, 8..15 tomorrow. Peak rather
      // than mean, because the question the row answers is "will I get wet".
      gRainTd = gRainTm = -1;
      for (int i = 0; i < 16 && i < gP3hN; i++) {
        int& peak = (i < 8) ? gRainTd : gRainTm;
        if ((int)gP3h[i] > peak) peak = gP3h[i];
      }

      Serial.printf("[meteo] now %.1f C icon(V2) %d  today %.0f-%.0f/%d%%  tmr %.0f-%.0f/%d%%  series %d/%d\n",
                    gOutTemp, gOutIcon, gTdMin, gTdMax, gRainTd,
                    gTmMin, gTmMax, gRainTm, gT1hN, gP3hN);
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

// RuuviTag coin cell (CR2477) is low around ~2.5 V; call 3.0 V a full cell.
#define LOW_BATT_MV  2500
#define FULL_BATT_MV 3000

// ===================== Weather Screen 320x240 =====================
// Imported from the "Weather Screen 320x240" design canvas, 1:1 -- 320x240 with
// no scaling, a 10 px side margin and 1 px hairlines at 32% ink. Bands:
//
//    y   0 .. 33   header       city + date, accent tick + clock
//    y  33 .. 34   hairline
//    y  34 ..130   hero         current outdoor temp, condition, H/L, icon panel
//    y 130 ..203   hourly grid  6 columns, 1 px border and 1 px gaps
//    y 203 ..210   gap
//    y 210 ..240   beacon strip the RuuviTag's live values, on a dark field
//
// The design specifies type in CSS px, which is NOT the glyph height. Measured
// from the shipped Barlow fonts, caps are 0.700 em and lining digits 0.718 em,
// so a 62 px hero number is a ~44.6 px glyph and a 22 px clock is ~15.8 px.
// The u8g2 "logisoso<N>" fonts are named by their digit height, and the
// "helv<W><N>" fonts by their cap height, so the picks below are the measured
// match -- using logisoso62 for the hero would have been ~40% oversized.
static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static constexpr uint16_t C_BG     = rgb565(242, 242, 243);  // #f2f2f3
static constexpr uint16_t C_INK    = rgb565( 29,  31,  32);  // #1d1f20
static constexpr uint16_t C_MUTED  = rgb565(122, 122, 125);  // #7a7a7d
static constexpr uint16_t C_MUTED3 = rgb565( 93,  93,  96);  // #5d5d60 neutral-700
static constexpr uint16_t C_ACCENT = rgb565( 65,  97, 128);  // #416180
static constexpr uint16_t C_BAR    = rgb565( 53,  89, 122);  // #35597a, 5.1:1 on the track
static constexpr uint16_t C_ACCENT2= rgb565( 53,  89, 122);  // #35597a, the rain value
static constexpr uint16_t C_ACC_LT = rgb565(148, 188, 227);  // #94bce3
static constexpr uint16_t C_FIELD  = rgb565( 29,  45,  61);  // #1d2d3d
static constexpr uint16_t C_TRACK  = rgb565(212, 212, 215);  // #d4d4d7
// The design puts hairlines at 32% ink, already raised from 16% because "a
// lighter 1 px rule disappears". On the actual ST7789 that reasoning holds
// further: 32% (2.0:1) still washed out, so the rules run at 60% (4.4:1).
static constexpr uint16_t C_RULE   = rgb565(114, 115, 116);
// Stale beacon: the design says the field "drops to accent-800". Read here as
// losing its accent -- the steel goes flat and the accent text goes neutral.
static constexpr uint16_t C_FIELD_STALE = rgb565(51, 56, 61);

// Weather-figure palette. The design strokes the figure in one accent blue;
// these are colour instead. A bright fill cannot carry contrast on a near-white
// ground -- a true sunny gold is only 1.6:1 -- so every shape is a bright fill
// inside a dark outline, and the outline is what makes it readable (all edge
// colours clear 4:1 on #f2f2f3). The outline is free: drawing the same shape
// one pixel larger underneath produces it, seams and all.
static constexpr uint16_t C_SUN_F   = rgb565(245, 184,  33);
static constexpr uint16_t C_SUN_E   = rgb565(179,  92,   0);
static constexpr uint16_t C_CLOUD_F = rgb565(168, 184, 201);
static constexpr uint16_t C_CLOUD_E = rgb565( 74,  92, 110);
static constexpr uint16_t C_RAIN_C  = rgb565( 63, 127, 196);
static constexpr uint16_t C_SNOW_C  = rgb565( 78, 134, 189);
static constexpr uint16_t C_FOG_C   = rgb565(124, 132, 140);
static constexpr uint16_t C_MOON_F  = rgb565(237, 227, 184);
static constexpr uint16_t C_MOON_E  = rgb565(154, 124,  34);
static constexpr uint16_t C_ACC_STALE   = rgb565(138, 143, 148);

static constexpr int16_t PADX    = 10;
static constexpr int16_t HDR_H   = 33;
static constexpr int16_t HERO_Y  = 34,  HERO_H  = 92;
static constexpr int16_t GRID_Y  = 126, GRID_H  = 77;
static constexpr int16_t GAP_H   = 7;
static constexpr int16_t FIELD_Y = 210, FIELD_H = 30;
static_assert(HDR_H + 1 + HERO_H + GRID_H + GAP_H + FIELD_H == 240,
              "screen bands must add up to the 240 px panel");
static_assert(FIELD_Y == HDR_H + 1 + HERO_H + GRID_H + GAP_H, "beacon strip offset");

// Stale rules from the design's build notes.
#define STALE_MS (5UL  * 60UL * 1000UL)   // age label starts reading the real gap
#define DEAD_MS  (30UL * 60UL * 1000UL)   // values dash out

// ---- text helpers ------------------------------------------------------
// Arduino_GFX has no letter-spacing, and the design tracks every uppercase
// label, so step glyph by glyph. Spaces get an explicit advance because
// getTextBounds reports no width for a lone space.
static uint16_t textWTracked(const char* s, int16_t track, int16_t spacePx) {
  uint16_t w = 0; int n = 0;
  for (const char* p = s; *p; ) {
    int len = 1;
    if      ((*p & 0xF8) == 0xF0) len = 4;
    else if ((*p & 0xF0) == 0xE0) len = 3;
    else if ((*p & 0xE0) == 0xC0) len = 2;
    if (*p == ' ') { w += spacePx; }
    else { char c[5] = {0}; for (int i = 0; i < len && p[i]; i++) c[i] = p[i]; w += textAdv(c); }
    p += len; n++;
  }
  return w + (n > 1 ? (n - 1) * track : 0);
}
static void printTracked(const char* s, int16_t x, int16_t y, int16_t track, int16_t spacePx) {
  for (const char* p = s; *p; ) {
    int len = 1;
    if      ((*p & 0xF8) == 0xF0) len = 4;
    else if ((*p & 0xF0) == 0xE0) len = 3;
    else if ((*p & 0xE0) == 0xC0) len = 2;
    if (*p == ' ') { x += spacePx + track; p += len; continue; }
    char c[5] = {0}; for (int i = 0; i < len && p[i]; i++) c[i] = p[i];
    gfx->setCursor(x, y); gfx->print(c);
    x += textAdv(c) + track;
    p += len;
  }
}
static void printAt(const char* s, int16_t x, int16_t baselineY) {
  gfx->setCursor(x, baselineY); gfx->print(s);
}
// sets the ink flush to rightX; returns the ink's left edge
static int16_t printRight(const char* s, int16_t rightX, int16_t baselineY) {
  int16_t lead; uint16_t w = textExt(s, &lead);
  gfx->setCursor(rightX - lead - (int16_t)w, baselineY); gfx->print(s);
  return rightX - (int16_t)w;
}
// centres the ink within [left, left+width)
static void printCentre(const char* s, int16_t left, int16_t width, int16_t baselineY) {
  int16_t lead; uint16_t w = textExt(s, &lead);
  gfx->setCursor(left + (width - (int16_t)w) / 2 - lead, baselineY); gfx->print(s);
}

// ---- condition classification -----------------------------------------
// MeteoSwiss DOES publish its symbol table -- it is linked off the
// weather-symbols page as "Weather symbols (XLSX)", dated 14 February 2022, and
// carries all 42 day codes with DE/FR/IT/EN text. Codes >= 100 are the night
// twins of the same weather. This table is transcribed from that file, so it is
// authoritative rather than inferred; in particular codes 12, 13, 23, 24, 25
// and 36-42 are all thunderstorms, which no amount of correlating sunshine
// against precipitation would have revealed.
static Cond condFromIcon(int icon, int rainProb, bool* night) {
  static const uint8_t kCond[42] = {
    COND_CLEAR,   COND_FAIR,    COND_PARTLY,  COND_CLOUDY,  COND_OVERCAST,  //  1- 5
    COND_SHOWERS, COND_SLEET,   COND_SNOW,    COND_SHOWERS, COND_SLEET,     //  6-10
    COND_SNOW,    COND_THUNDER, COND_THUNDER, COND_RAIN,    COND_SLEET,     // 11-15
    COND_SNOW,    COND_RAIN,    COND_SLEET,   COND_SNOW,    COND_RAIN,      // 16-20
    COND_SLEET,   COND_SNOW,    COND_THUNDER, COND_THUNDER, COND_THUNDER,   // 21-25
    COND_FAIR,    COND_FOG,     COND_FOG,     COND_SHOWERS, COND_SNOW,      // 26-30
    COND_SLEET,   COND_SHOWERS, COND_RAIN,    COND_SNOW,    COND_CLOUDY,    // 31-35
    COND_THUNDER, COND_THUNDER, COND_THUNDER, COND_THUNDER, COND_THUNDER,   // 36-40
    COND_THUNDER, COND_THUNDER,                                             // 41-42
  };
  *night = (icon >= 100);
  const int d = icon % 100;
  if (d >= 1 && d <= 42) return (Cond)kCond[d - 1];
  // outside the published range: fall back on something always trustworthy
  if (rainProb >= 70) return COND_RAIN;
  if (rainProb >= 45) return COND_SHOWERS;
  if (rainProb >= 10) return COND_CLOUDY;
  return COND_FAIR;
}
static const char* condText(Cond c) {
  switch (c) {
    case COND_CLEAR:    return "CLEAR";
    case COND_FAIR:     return "MOSTLY SUNNY";
    case COND_PARTLY:   return "PARTLY CLOUDY";
    case COND_CLOUDY:   return "CLOUDY";
    case COND_OVERCAST: return "OVERCAST";
    case COND_FOG:      return "FOG";
    case COND_SHOWERS:  return "SHOWERS";
    case COND_RAIN:     return "RAIN";
    case COND_SLEET:    return "SLEET";
    case COND_SNOW:     return "SNOW";
    default:            return "THUNDERSTORMS";
  }
}

// ---- pictograms --------------------------------------------------------
// The design's icon is a 1.5 px stroked SVG. Its own build notes point out that
// a 1 px rule disappears on this panel, and the same is true of a stroked icon,
// so these are filled silhouettes in the accent colour. Where the sun sits
// behind the cloud the cloud is drawn twice -- once oversized in background,
// once at size -- which leaves a clean 2 px separation instead of a collision.
static void icoSun(int16_t cx, int16_t cy, int16_t r) {
  for (int i = 0; i < 8; i++) {                       // rays, in the edge tone
    float a = i * (float)M_PI / 4.0f;
    gfx->drawLine(cx + (int16_t)(cosf(a) * (r + 3)), cy + (int16_t)(sinf(a) * (r + 3)),
                  cx + (int16_t)(cosf(a) * (r + 6)), cy + (int16_t)(sinf(a) * (r + 6)), C_SUN_E);
  }
  gfx->fillCircle(cx, cy, r, C_SUN_E);                // one px larger = outline
  gfx->fillCircle(cx, cy, r - 1, C_SUN_F);
}
static void icoMoon(int16_t cx, int16_t cy, int16_t r) {
  gfx->fillCircle(cx, cy, r, C_MOON_E);
  gfx->fillCircle(cx, cy, r - 1, C_MOON_F);
  gfx->fillCircle(cx + r / 2, cy - r / 2, r, C_BG);   // bite the crescent out
  gfx->drawCircle(cx + r / 2, cy - r / 2, r, C_MOON_E);
}
static void icoCloud(int16_t x, int16_t y, int16_t w, uint16_t col, int16_t grow) {
  int16_t h  = (int16_t)(w * 0.70f);
  int16_t rb = (int16_t)(w * 0.30f) + grow;
  int16_t rs = (int16_t)(w * 0.22f) + grow;
  int16_t by = y + (int16_t)(h * 0.55f) - grow;
  int16_t bh = (int16_t)(h * 0.45f) + 2 * grow;
  gfx->fillCircle(x + (int16_t)(w * 0.40f), y + (int16_t)(h * 0.42f), rb, col);
  gfx->fillCircle(x + (int16_t)(w * 0.72f), y + (int16_t)(h * 0.55f), rs, col);
  gfx->fillRoundRect(x - grow, by, w + 2 * grow, bh, rs / 2, col);
}
static void icoBolt(int16_t x, int16_t y) {
  gfx->fillTriangle(x + 4, y - 8, x - 5, y + 3, x + 2, y + 3, C_SUN_E);
  gfx->fillTriangle(x - 4, y + 8, x + 5, y - 3, x - 2, y - 3, C_SUN_E);
  gfx->fillTriangle(x + 3, y - 6, x - 3, y + 2, x + 1, y + 2, C_SUN_F);
  gfx->fillTriangle(x - 3, y + 6, x + 3, y - 2, x - 1, y - 2, C_SUN_F);
}
static void icoFlake(int16_t x, int16_t y) {
  gfx->drawFastHLine(x - 2, y, 5, C_SNOW_C);
  gfx->drawFastVLine(x, y - 2, 5, C_SNOW_C);
}
static void drawCondIcon(int16_t cx, int16_t cy, Cond c, bool night) {
  if (c == COND_FOG) {
    for (int i = 0; i < 4; i++)
      gfx->drawFastHLine(cx - 18 + (i & 1) * 4, cy - 12 + i * 8, 32 - (i & 1) * 6, C_FOG_C);
    return;
  }
  const bool hasCloud = (c != COND_CLEAR);
  const bool hasLight = (c == COND_CLEAR || c == COND_FAIR || c == COND_PARTLY);
  if (hasLight) {
    int16_t sx = hasCloud ? cx + 13 : cx;
    int16_t sy = hasCloud ? cy - 12 : cy;
    if (night) icoMoon(sx, sy, hasCloud ? 9 : 13);
    else       icoSun (sx, sy, hasCloud ? 8 : 12);
  }
  if (hasCloud) {
    icoCloud(cx - 22, cy - 10, 44, C_BG,      3);   // gap, so the sun stays clear
    icoCloud(cx - 22, cy - 10, 44, C_CLOUD_E, 1);   // outline
    icoCloud(cx - 22, cy - 10, 44, C_CLOUD_F, 0);   // body
    if (c == COND_THUNDER) {
      icoBolt(cx, cy + 21);
    } else if (c == COND_SNOW || c == COND_SLEET) {
      for (int i = 0; i < 3; i++) icoFlake(cx - 12 + i * 12, cy + 20);
      if (c == COND_SLEET) gfx->drawLine(cx + 6, cy + 17, cx + 4, cy + 23, C_RAIN_C);
    } else {
      const int drops = (c == COND_SHOWERS) ? 2 : (c == COND_RAIN) ? 3 : 0;
      for (int i = 0; i < drops; i++) {
        int16_t dx = cx - 15 + i * 10;
        gfx->drawLine(dx,     cy + 18, dx - 2, cy + 24, C_RAIN_C);
        gfx->drawLine(dx + 1, cy + 18, dx - 1, cy + 24, C_RAIN_C);
      }
    }
  }
}

// Lucide "battery": rounded body plus the terminal nub, 10 px wide
static void icoBattery(int16_t x, int16_t cy, uint16_t col) {
  gfx->drawRoundRect(x, cy - 3, 9, 6, 1, col);
  gfx->drawFastVLine(x + 9, cy - 1, 2, col);
}

// small right-pointing arrow, 7 px wide, centred vertically on y
static void drawArrow(int16_t x, int16_t y, uint16_t col) {
  gfx->drawFastHLine(x, y, 7, col);
  gfx->drawLine(x + 4, y - 3, x + 6, y, col);
  gfx->drawLine(x + 4, y + 3, x + 6, y, col);
}

// the design's blueprint corner ticks: a 7 px L at each corner of a panel
static void cornerTicks(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t col) {
  const int16_t t = 7, o = 3;
  gfx->drawFastHLine(x - o,         y - o, t, col);  gfx->drawFastVLine(x - o,         y - o, t, col);
  gfx->drawFastHLine(x + w - t + o, y - o, t, col);  gfx->drawFastVLine(x + w + o - 1, y - o, t, col);
  gfx->drawFastHLine(x - o,   y + h + o - 1, t, col); gfx->drawFastVLine(x - o,         y + h - t + o, t, col);
  gfx->drawFastHLine(x + w - t + o, y + h + o - 1, t, col);
  gfx->drawFastVLine(x + w + o - 1, y + h - t + o, t, col);
}

// Slide the HOURS-wide window onto the cached series using the current clock.
// Cheap enough to run on every redraw, which is what keeps the strip current:
// previously this lived inside fetchMeteo(), so the columns only moved every
// 30 minutes and stayed empty entirely if the fetch beat the NTP sync.
static void rebuildHours() {
  gHaveHours = false;
  gRainProb  = -1;
  time_t now = time(nullptr);
  if (gGraphStart == 0 || now < 1600000000 || gT1hN == 0) return;
  long idx = (long)((now - gGraphStart) / 3600);
  if (idx < 0 || idx + HOURS > gT1hN) return;
  for (int i = 0; i < HOURS; i++) {
    long h = idx + i;
    gHours[i].temp = gT1h[h];
    long slot = h / 3;
    gHours[i].pop = (slot < gP3hN) ? gP3h[slot] : 0;
    time_t    ts = gGraphStart + h * 3600;
    struct tm lt;
    localtime_r(&ts, &lt);          // follows the DST rule, unlike a fixed offset
    snprintf(gHours[i].label, sizeof(gHours[i].label), "%02d", lt.tm_hour);
  }
  long slotNow = idx / 3;
  gRainProb = (slotNow < gP3hN) ? (int)gP3h[slotNow] : -1;
  gHaveHours = true;
}

// ---- the screen --------------------------------------------------------
static void drawScreen() {
  static bool hadHours = false;
  rebuildHours();
  if (gHaveHours != hadHours) {          // so the fill can be timed from boot
    hadHours = gHaveHours;
    Serial.printf("[meteo] hourly strip %s\n", gHaveHours ? "filled" : "cleared");
  }
  gfx->fillScreen(C_BG);
  char buf[48];

  struct tm ti;
  const bool haveClock = getLocalTime(&ti, 5);

  // ---------------- header ----------------
  // city, uppercased; ip-api gives Latin-1-able names and the helv *_tf fonts
  // carry them, so only ASCII is case-folded and anything else passes through.
  char city[32];
  // An explicit plz does not change the IP-derived city name, and MeteoSwiss
  // has no plz -> place lookup, so the label is its own setting.
  snprintf(city, sizeof(city), "%s",
           cfgCity.length() ? cfgCity.c_str() : (gCity[0] ? gCity : "--"));
  for (char* q = city; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
  gfx->setFont(u8g2_font_helvB10_tf);
  gfx->setTextColor(C_INK);
  printTracked(city, PADX, 27, 2, 4);
  int16_t cityW = (int16_t)textWTracked(city, 2, 4);

  if (haveClock) {
    static const char* WD[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
    snprintf(buf, sizeof(buf), "%s %02d.%02d", WD[ti.tm_wday % 7], ti.tm_mday, ti.tm_mon + 1);
    gfx->setFont(u8g2_font_helvR08_tf);
    gfx->setTextColor(C_MUTED3);
    printTracked(buf, PADX + cityW + 7, 27, 1, 3);
  }

  // clock, right-aligned, with the accent tick beside it
  snprintf(buf, sizeof(buf), "%s", haveClock ? "" : "--:--");
  if (haveClock) strftime(buf, sizeof(buf), "%H:%M", &ti);
  gfx->setFont(u8g2_font_logisoso16_tn);
  gfx->setTextColor(C_INK);
  int16_t clkX = printRight(buf, 320 - PADX, 27);
  gfx->fillRect(clkX - 6 - 5, 17, 5, 5, C_ACCENT);

  gfx->drawFastHLine(PADX, HDR_H, 320 - 2 * PADX, C_RULE);

  // ---------------- hero ----------------
  // Three columns: the numeral, a spec column of hairline-ruled rows, and the
  // condition figure. The spec column gets whatever the other two leave -- about
  // 101 px. Barlow Condensed fits the design's 14 px title in that; helv is
  // ~30% wider at the same cap height and would need 129 px, so the title uses
  // helvB08 and keeps its hierarchy over the row labels by weight (bold vs
  // regular) rather than by size, which is how the design separates them too.
  const int16_t heroTop = HERO_Y + 6, heroBot = HERO_Y + HERO_H - 4;   // 40 .. 122
  const int16_t panelW = 80, panelX = 320 - PADX - panelW;             // 230
  const int16_t panelY = heroTop, panelH = heroBot - heroTop;          // 40, 82

  bool night = false;
  Cond cond = condFromIcon(gOutIcon, gRainProb, &night);

  // The headline number is the RuuviTag's OWN measurement -- that is the point
  // of the device, and a sensor on the balcony beats a regional figure. Rounded
  // to a whole degree: the design's numeral is integer-styled, and "18.4" at
  // this size would eat the spec column. The tenth still shows on the strip.
  if (haveReading) snprintf(buf, sizeof(buf), "%.0f", gTempC);
  else             snprintf(buf, sizeof(buf), "--");
  const int16_t numTop = heroTop + (panelH - 54) / 2;
  gfx->setFont(u8g2_font_logisoso54_tn);
  gfx->setTextColor(C_INK);
  printAt(buf, PADX, numTop + 54);
  int16_t numW = textAdv(buf);

  gfx->setFont(u8g2_font_helvB18_tf);
  gfx->setTextColor(C_ACCENT);
  printAt("°C", PADX + numW + 2, numTop + 19);
  numW += 2 + textAdv("°C");

  if (gHaveWx) {
    // --- spec column: everything MeteoSwiss contributes ---
    const int16_t specX = PADX + numW + 8;
    const int16_t specT = specX + 9;              // 1 px rule + 8 px padding
    const int16_t specR = panelX - 8;
    gfx->drawFastVLine(specX, panelY, panelH, C_RULE);

    gfx->setFont(u8g2_font_helvB08_tf);
    gfx->setTextColor(C_INK);
    printTracked(condText(cond), specT, 53, 1, 3);

    static const int16_t rowY[3] = { 59, 81, 103 };
    static const char*   rowL[3] = { "HIGH", "LOW", "RAIN" };
    for (int r = 0; r < 3; r++) {
      gfx->drawFastHLine(specT, rowY[r], specR - specT, C_RULE);
      const int16_t base = rowY[r] + 17;
      gfx->setFont(u8g2_font_helvR08_tf);
      gfx->setTextColor(C_MUTED3);
      printTracked(rowL[r], specT, base, 1, 3);
      // Every row reads today -> tomorrow on one line. All three are daily
      // figures, so rain is the day's PEAK chance, not the current 3 h slot --
      // a "right now" number would not belong beside a daily high and low.
      char tod[8], tmr[8];
      bool haveT = gHaveTmr;
      if (r == 0) {
        snprintf(tod, sizeof(tod), "%.0f°", gTdMax);
        snprintf(tmr, sizeof(tmr), "%.0f°", gTmMax);
      } else if (r == 1) {
        snprintf(tod, sizeof(tod), "%.0f°", gTdMin);
        snprintf(tmr, sizeof(tmr), "%.0f°", gTmMin);
      } else {
        if (gRainTd >= 0) snprintf(tod, sizeof(tod), "%d%%", gRainTd);
        else              snprintf(tod, sizeof(tod), "--");
        if (gRainTm >= 0) snprintf(tmr, sizeof(tmr), "%d%%", gRainTm);
        else              haveT = false;
      }
      // laid out right to left, so tomorrow sits flush and today keeps emphasis
      int16_t rx = specR;
      gfx->setFont(u8g2_font_helvB12_tf);
      if (haveT) {
        gfx->setTextColor(C_MUTED3);
        rx = printRight(tmr, rx, base) - 3;
        drawArrow(rx - 7, base - 4, C_MUTED3);
        rx -= 7 + 3;
      }
      gfx->setTextColor(r == 2 ? C_ACCENT2 : C_INK);
      printRight(tod, rx, base);
    }

    // --- condition figure ---
    gfx->drawRect(panelX, panelY, panelW, panelH, C_RULE);
    cornerTicks(panelX, panelY, panelW, panelH, C_MUTED);
    drawCondIcon(panelX + panelW / 2, panelY + panelH / 2, cond, night);
  } else {
    // No forecast: the measurement still stands on its own, so keep the numeral
    // and use the space the spec column and figure would have taken to say why
    // the rest is missing, rather than drawing an empty frame.
    const char* msg = (WiFi.status() != WL_CONNECTED)
                    ? (cfgWifi.length() ? "WIFI FAILED - CHECK USB" : "NO WIFI - SET OVER USB")
                    : "WAITING FOR FORECAST";
    gfx->setFont(u8g2_font_helvB08_tf);
    gfx->setTextColor(C_ACCENT);
    printTracked(msg, PADX + numW + 8, numTop + 30, 1, 3);
  }

  // ---------------- hourly grid ----------------
  // The rule colour is painted as the background, then each cell is knocked
  // back out in page colour -- which is exactly how the design builds it, and
  // gives the 1 px border and 1 px gaps for free. Note the bars come in threes:
  // MeteoSwiss publishes precipitation probability 3-hourly, so nine hourly
  // columns are exactly three blocks. The temperatures ARE hourly and distinct.
  gfx->fillRect(PADX, GRID_Y, 320 - 2 * PADX, GRID_H, C_RULE);
  for (int i = 0; i < HOURS; i++) {
    // Tile 298 px of inner width into HOURS cells separated by 1 px gaps, by
    // walking the exact boundaries so rounding never opens or closes a seam.
    const int16_t a  = PADX + 1 + (int16_t)((int32_t)i * 298 / HOURS);
    const int16_t b  = PADX + 1 + (int16_t)((int32_t)(i + 1) * 298 / HOURS) - 1;
    const int16_t cl = a;
    const int16_t cw = b - a + (i == HOURS - 1 ? 1 : 0);
    gfx->fillRect(cl, GRID_Y + 1, cw, GRID_H - 2, C_BG);

    const bool ok = gHaveHours;
    gfx->setFont(u8g2_font_helvR08_tf);
    gfx->setTextColor(C_MUTED3);
    snprintf(buf, sizeof(buf), "%s", ok ? gHours[i].label : "--");
    printCentre(buf, cl, cw, 140);

    gfx->setFont(u8g2_font_helvB12_tf);
    gfx->setTextColor(C_INK);
    if (ok) snprintf(buf, sizeof(buf), "%.0f°", gHours[i].temp);
    else    snprintf(buf, sizeof(buf), "--");
    printCentre(buf, cl, cw, 160);

    // Precipitation probability, 20 px full scale and truly proportional --
    // no minimum bar, because the printed % carries anything under 15%.
    const int16_t bx = cl + (cw - 14) / 2;
    gfx->fillRect(bx, 165, 14, 20, C_TRACK);
    if (ok) {
      int16_t bh = (int16_t)((gHours[i].pop * 20 + 50) / 100);
      if (bh < 0)  bh = 0;
      if (bh > 20) bh = 20;
      if (bh) gfx->fillRect(bx, 185 - bh, 14, bh, C_BAR);
    }

    gfx->setFont(u8g2_font_helvR08_tf);
    gfx->setTextColor(C_MUTED3);
    if (ok) snprintf(buf, sizeof(buf), "%d%%", gHours[i].pop);
    else    snprintf(buf, sizeof(buf), "--");
    printCentre(buf, cl, cw, 197);
  }

  // ---------------- beacon strip ----------------
  // Only live values belong here, so a stale reading can never be mistaken for
  // part of the forecast above it.
  const uint32_t age  = haveReading ? (millis() - gLastMs) : 0xFFFFFFFF;
  const bool     dead = !haveReading || age > DEAD_MS;
  const bool     stale = !haveReading || age > STALE_MS;
  const uint16_t fieldCol = stale ? C_FIELD_STALE : C_FIELD;
  const uint16_t accCol   = stale ? C_ACC_STALE   : C_ACC_LT;

  gfx->fillRect(0, FIELD_Y, 320, FIELD_H, fieldCol);

  // Broadcast mark: the design's SVG is a centre dot with a symmetric pair of
  // arcs either side, so draw two rings and mask the top and bottom away --
  // what survives is the left and right of each ring. The dot goes on last so
  // the masking cannot bite into it.
  const int16_t gy = FIELD_Y + FIELD_H / 2, gx = PADX + 5;
  gfx->drawCircle(gx, gy, 5, accCol);
  gfx->drawCircle(gx, gy, 8, accCol);
  gfx->fillRect(gx - 9, gy - 10, 19, 6, fieldCol);
  gfx->fillRect(gx - 9, gy + 5,  19, 6, fieldCol);
  gfx->fillCircle(gx, gy, 2, accCol);

  // Values first, right to left, so we know how much room is left for the
  // label. The group is not a fixed width -- in the stale state the age reads
  // "14m" rather than "2s" -- and a long label would otherwise run underneath
  // it. Measure, then fit the label to what is left.
  const int16_t vb = gy + 6;      // value baseline
  const int16_t VGAP = 8;         // between value groups
  const int16_t UGAP = 3;         // between a value and its unit
  int16_t x = 320 - PADX;

  gfx->setFont(u8g2_font_helvR08_tf);
  gfx->setTextColor(accCol);
  if (dead) snprintf(buf, sizeof(buf), "--");
  else {
    int pct = (int)(((int)gMv - LOW_BATT_MV) * 100 / (FULL_BATT_MV - LOW_BATT_MV));
    pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    snprintf(buf, sizeof(buf), "%d%%", pct);
  }
  x = printRight(buf, x, vb) - 4;
  icoBattery(x - 10, vb - 4, accCol);
  x -= 10 + VGAP;

  if (dead)               snprintf(buf, sizeof(buf), "--");
  else if (age < 60000UL) snprintf(buf, sizeof(buf), "%lus", (unsigned long)(age / 1000));
  else                    snprintf(buf, sizeof(buf), "%lum", (unsigned long)(age / 60000UL));
  x = printRight(buf, x, vb) - VGAP;

  gfx->setTextColor(accCol);
  x = printRight("hPa", x, vb) - UGAP;
  gfx->setFont(u8g2_font_helvB12_tf);
  gfx->setTextColor(dead ? accCol : C_BG);
  if (dead) snprintf(buf, sizeof(buf), "--");
  else      snprintf(buf, sizeof(buf), "%.0f", gHpa);
  x = printRight(buf, x, vb) - VGAP;

  gfx->setFont(u8g2_font_helvR08_tf);
  gfx->setTextColor(accCol);
  x = printRight("%RH", x, vb) - UGAP;
  gfx->setFont(u8g2_font_helvB12_tf);
  gfx->setTextColor(dead ? accCol : C_BG);
  if (dead) snprintf(buf, sizeof(buf), "--");
  else      snprintf(buf, sizeof(buf), "%.0f", gHum);
  x = printRight(buf, x, vb) - VGAP;

  const int16_t valuesLeft = x + VGAP;   // temperature is the hero; no duplicate

  // label, clipped to the space the values left behind
  gfx->setFont(u8g2_font_helvB08_tf);
  gfx->setTextColor(accCol);
  char name[24];
  snprintf(name, sizeof(name), "%s", cfgBeacon.length() ? cfgBeacon.c_str() : "BEACON");
  for (char* q = name; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
  const int16_t nameX = PADX + 18;
  const int16_t avail = valuesLeft - 6 - nameX;
  while (name[0] && (int16_t)textWTracked(name, 2, 3) > avail) {
    size_t n = strlen(name) - 1;
    while (n > 0 && (name[n] & 0xC0) == 0x80) n--;   // don't split a UTF-8 char
    name[n] = 0;
  }
  printTracked(name, nameX, gy + 4, 2, 3);

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
  Serial.println(F("  city            header label (blank = city name from IP geolocation)"));
  Serial.println(F("  beacon          label shown on the beacon strip (e.g. Balcony)"));
  Serial.println(F("examples:  wifi=YourSSID   |   plz=8802   |   beacon=Balcony\n"));
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
  struct tm ti;
  if (getLocalTime(&ti, 100))
    Serial.printf("time      = %04d-%02d-%02d %02d:%02d:%02d %s\n",
                  ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                  ti.tm_hour, ti.tm_min, ti.tm_sec, ti.tm_isdst > 0 ? "CEST" : "CET");
  else
    Serial.println(F("time      = (not synced)"));
  Serial.printf("city      = %s\n", cfgCity.length() ? cfgCity.c_str() : "(unset, uses IP city)");
  Serial.printf("beacon    = %s\n", cfgBeacon.length() ? cfgBeacon.c_str() : "(unset, shows BEACON)");
  Serial.printf("ble       = %lu adverts seen, %lu with mfg data\n",
                (unsigned long)gAdvSeen, (unsigned long)gAdvMfg);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("wifi state= connected, IP "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("wifi state= not connected"));
  }
  if (gGeoDone) Serial.printf("location  = %s (plz %d), UTC offset %lds\n", gCity, gPlz, gOffset);
  if (gHaveWx)  Serial.printf("weather   = now %.1f, today %.0f-%.0f rain %d%%, tomorrow %.0f-%.0f rain %d%%\n",
                              gOutTemp, gTdMin, gTdMax, gRainTd, gTmMin, gTmMax, gRainTm);
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
  } else if (key == "city") {
    cfgCity = val; prefs.putString("city", val);
    Serial.printf("[cfg] city = %s (saved)\n", val.c_str());
  } else if (key == "beacon") {
    cfgBeacon = val; prefs.putString("beacon", val);
    Serial.printf("[cfg] beacon = %s (saved)\n", val.c_str());
  } else if (key == "ble") {
    gBleDebug = (val.toInt() != 0);
    Serial.printf("[cfg] ble debug %s\n", gBleDebug ? "on" : "off");
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
  cfgBeacon = prefs.getString("beacon", "");
  cfgCity   = prefs.getString("city", "");

  // Clock from a timezone RULE, not a fixed offset, so the board follows the
  // March and October changes while it is running. ip-api's offset is a
  // snapshot taken at boot: a board left running across a change would read an
  // hour wrong until rebooted. Central European time is right by construction
  // here -- the forecast comes from MeteoSwiss and is keyed by a Swiss postal
  // code -- and the rule covers the whole EU zone.
  //   CET-1CEST  standard UTC+1 (POSIX inverts the sign), summer CEST
  //   M3.5.0     DST starts the last Sunday in March
  //   M10.5.0/3  and ends the last Sunday in October at 03:00
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com");

  WiFi.mode(WIFI_STA);
  // Record why a drop happened: a beacon timeout points at radio contention
  // with the BLE scan, an auth or no-AP reason points somewhere else entirely.
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    Serial.printf("[wifi] disconnected, reason %u\n",
                  (unsigned)info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
    Serial.println(F("[wifi] connected"));
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
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
  // A burst is only a few seconds long, so scan hard while it runs -- the
  // radio is handed back entirely between bursts, which is what protects
  // WiFi. Averaged over a minute this is a few percent, not 99%.
  scan->setWindow(70);
  // NimBLE filters duplicates by default, so a beacon would be reported once
  // and then never again -- useless for a live reading. Keep every repeat, and
  // stop it accumulating results in RAM since we only use the callback.
  scan->setDuplicateFilter(false);
  scan->setMaxResults(0);
  gScanActive = true;
  scan->start(SCAN_BURST_MS, false, true);
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
      // Retry quickly until BOTH the summary and the series are cached. Keying
      // this on gHaveWx alone meant a fetch landing before NTP left the strip
      // empty for the next half hour.
      uint32_t interval = (gHaveWx && gT1hN > 0) ? 1800000UL : 30000UL;
      if (lastMeteo == 0 || millis() - lastMeteo > interval) {
        lastMeteo = millis();
        fetchMeteo();
      }
    }
  }

  // --- BLE: one short burst, then give the radio back to WiFi ---
  {
    static uint32_t seqAtStart = 0;
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (gScanActive) {
      if (gSeq != seqAtStart) {        // reading in hand, no reason to keep listening
        scan->stop();
        gScanActive = false;
        gScanEnded  = millis();
      }
    } else {
      uint32_t quiet = (haveReading && millis() - gLastMs < 90000UL)
                     ? SCAN_FRESH_MS : SCAN_RETRY_MS;
      if (millis() - gScanEnded > quiet) {
        seqAtStart  = gSeq;
        gScanActive = true;
        scan->start(SCAN_BURST_MS, false, true);
      }
    }
  }

  // --- WiFi watchdog ---
  // The core reconnects on its own for a normal drop, so give it 30 s before
  // stepping in. Reconnecting the moment the link looks down would cancel an
  // attempt already in flight, including the one setup() just started.
  static uint32_t wifiDownSince = 0, lastWifiKick = 0;
  if (WiFi.status() == WL_CONNECTED) {
    wifiDownSince = 0;
  } else if (cfgWifi.length()) {
    uint32_t t = millis();
    if (wifiDownSince == 0) wifiDownSince = t;
    else if (t - wifiDownSince > 30000 && t - lastWifiKick > 30000) {
      lastWifiKick = t;
      Serial.println(F("[wifi] still down after 30 s, forcing a reconnect"));
      WiFi.disconnect();
      WiFi.begin(cfgWifi.c_str(), cfgPass.c_str());
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
  } else if (beaconPresent && nowMs - lastReadingMs > 180000) {
    // Readings now arrive about once a minute, one per scan burst, so this
    // has to tolerate a missed burst or two without declaring the beacon lost
    // and re-playing the chime. Still inside the 5-minute stale rule.
    beaconPresent = false;
  }

  // --- redraw on a new reading, or once a second so the clock ticks ---
  if (newReading || nowMs - lastClock > 1000) {
    lastClock = nowMs;
    lastDrawn = gSeq;
    drawScreen();
  }
  delay(100);
}
