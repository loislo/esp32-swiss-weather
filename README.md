# RuuviBeacon

A standalone desk thermometer: a **Waveshare ESP32-S3-Touch-LCD-2.8** reads a
**RuuviTag** over Bluetooth and shows it on its own 2.8″ colour touchscreen —
no phone or computer needed once it's running.

## Features

- **Indoor temperature** from a RuuviTag (BLE, data format 5), shown big and
  **colour-coded** (freezing blue → teal → yellow → red).
- **Top bar:** NTP clock (left) and **tomorrow's** min–max (right).
- **Today's** outdoor min–max under the big number.
- **One status line:** humidity, pressure (mmHg), beacon battery (V), signal (dBm).
- **Warnings** (second line from bottom): red *"Beacon battery low"* (< 2.5 V),
  amber *"No WiFi…"* with a setup hint.
- **Chime** through the onboard PCM5101 speaker when the beacon is (re)acquired.
- **Touch:** drag a finger up/down to brighten/dim the backlight.
- **Auto-location over WiFi:** IP geolocation (`ip-api.com`) sets the timezone and
  picks your postal code, which feeds **MeteoSwiss** for the forecast — nothing
  about your location is hardcoded.
- **Serial config console:** set WiFi/etc. as `key=value` over USB; saved to flash.
- Flicker-free rendering via a PSRAM double-buffer.

## Hardware

| | |
|---|---|
| Board | Waveshare ESP32-S3-Touch-LCD-2.8 (ESP32-S3, 16 MB flash, 8 MB PSRAM) |
| Display | 2.8″ IPS 240×320, **ST7789** (SPI) |
| Touch | **CST328** 5-point capacitive (I²C) |
| Audio | **PCM5101** I²S DAC → 8 Ω speaker |
| Sensor | RuuviTag (external, BLE) |

Pin map and register details are in the source header comments
(`firmware/ruuvi_display/ruuvi_display.ino`).

## Repo layout

```
firmware/ruuvi_display/   main firmware (display, weather, audio, touch, config)
firmware/ruuvi_scan/      minimal serial-only scanner (early baseline)
ruuvi_scan.py             host-side RuuviTag scanner (Python/bleak) for the Mac
case/                     parametric OpenSCAD enclosure (draft — verify dims)
```

(The 16 MB factory-firmware backup and the Python `.venv` are git-ignored.)

## Build & flash

Uses [`arduino-cli`](https://arduino.github.io/arduino-cli/) with the ESP32 core.

```bash
# one-time: core + libraries
arduino-cli core install esp32:esp32          # 3.3.10
arduino-cli lib install "NimBLE-Arduino" "GFX Library for Arduino" "U8g2" "ArduinoJson"

# compile + flash  (port varies; check: ls /dev/cu.usbmodem*)
FQBN="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,PartitionScheme=huge_app"
arduino-cli compile --fqbn "$FQBN" firmware/ruuvi_display
arduino-cli upload -p /dev/cu.usbmodem1101 --fqbn "$FQBN" firmware/ruuvi_display
```

**Reading serial on this board:** opening the USB-Serial/JTAG port with RTS
asserted holds the chip in reset. Open with RTS **and** DTR de-asserted (e.g.
`pyserial` with `rts=False; dtr=False` before `open()`), or use the Arduino
monitor which handles it.

## First-run configuration (serial console)

WiFi credentials are **not** in the source — they live in the device's flash
(NVS) and are set over USB. Open a serial monitor at 115200 and type:

```
help                     # list commands + keys
show                     # current values (temp, wifi, location, weather)
wifi=YourSSID            # set WiFi SSID
wifi_pass=YourPassword   # set WiFi password (saved; device reconnects)
plz=8802                 # optional: force a postal code (0 = auto from IP)
```

Settings persist across reboots and reflashes. Without WiFi the device still
shows the RuuviTag temperature; the clock/weather come online once connected.

## Data sources

- Indoor temp/humidity/pressure/battery: the RuuviTag BLE advertisement.
- Timezone + postal code: `http://ip-api.com/json` (uses the device's public IP).
- Forecast: `https://app-prod-ws.meteoswiss-app.ch/v1/forecast?plz=<plz>00`
  (`regionForecast[0]`/`[1]` → today/tomorrow min–max).

## Case

`case/ruuvibeacon_case.scad` is a **parametric** two-part enclosure (back tray +
front bezel, friction fit). Outer board size (73.06 × 50.54 mm) is from
Waveshare; component heights, screen-window position, and USB-C location are
estimates — **measure your board and adjust the variables at the top** before
printing. Render:

```bash
openscad -D 'part="back"'  -o ruuvibeacon_back.stl  case/ruuvibeacon_case.scad
openscad -D 'part="bezel"' -o ruuvibeacon_bezel.stl case/ruuvibeacon_case.scad
```

## Restore the factory demo

The original Waveshare firmware was backed up before flashing:

```bash
esptool write-flash 0x0 waveshare-factory-backup.bin
```
