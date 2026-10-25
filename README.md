# RuuviBeacon

A standalone desk thermometer: a **Waveshare ESP32-S3-Touch-LCD-2.8** reads a
**RuuviTag** over Bluetooth and shows it on its own 2.8″ colour touchscreen —
no phone or computer needed once it's running.

## Features

The display runs the **Weather Screen 320x240** design: a landscape weather
screen with the RuuviTag on a strip along the foot.

- **The RuuviTag's own measurement, big** — a sensor on your balcony beats a
  regional figure. Shown to a whole degree; the tenth is on the strip below.
- **Alongside it**, from MeteoSwiss: the condition, and `HIGH` / `LOW` / `RAIN`
  rows that each read **today → tomorrow**, plus a condition pictogram.
- **Nine-hour outlook** across a hairline grid — hour, temperature, and a
  precipitation-probability bar at 20 px full scale. The temperatures are
  hourly; the bars come in threes, because MeteoSwiss publishes precipitation
  probability 3-hourly.
- **Header:** city, date, and NTP clock. The clock follows a timezone **rule**,
  so it survives the March and October changes without a reboot.
- **Beacon strip** (the dark field at the bottom): the RuuviTag's humidity,
  pressure, reading age and battery. Only live values sit here, so a stale
  reading can never be read as part of the forecast above it.
- **Stale handling:** past 5 minutes the age label reads the real gap and the
  strip loses its accent; past 30 minutes the values dash out.
- **Chime** through the onboard PCM5101 speaker when the beacon is (re)acquired.
- **Touch:** drag a finger up/down to brighten/dim the backlight.
- **Auto-location over WiFi:** IP geolocation (`ip-api.com`) picks your postal
  code, which feeds **MeteoSwiss**. Override it with `plz=` when the IP lands
  in the next village, and `city=` for the header label.
- **Radio sharing:** WiFi and BLE share one 2.4 GHz antenna, so the tag is
  scanned in short bursts rather than continuously — a permanent scan starves
  the WiFi link.
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
case/                     OpenSCAD enclosure + measured board dimensions
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
beacon=Balcony           # label shown on the beacon strip (default: BEACON)
```

Settings persist across reboots and reflashes. Without WiFi the device still
shows the RuuviTag temperature; the clock/weather come online once connected.

## Data sources

- Beacon temp/humidity/pressure/battery: the RuuviTag BLE advertisement.
- Timezone + postal code: `http://ip-api.com/json` (uses the device's public IP).
- Forecast: `https://app-prod-ws.meteoswiss-app.ch/v1/plzDetail?plz=<plz>00`

The screen uses **`plzDetail`**, not the `forecast` endpoint: it needs
`graph.precipitationProbability3h` for the hero's rain figure and the hourly
bars, and that array exists only on `plzDetail`. Fields read are
`currentWeather.temperature`/`.iconV2`, `forecast[0..1].temperatureMin`/`Max`,
and `graph.start`/`.temperatureMean1h`/`.precipitationProbability3h`. The
response carries about twenty arrays of 144 floats, so it is parsed through an
ArduinoJson filter and streamed straight from the socket.

`graph.start` is local midnight as an absolute epoch and the samples are hourly
from there, so the sample for a given moment is `(now - start) / 1h` and its
probability slot is that index over three.

**Condition labels.** MeteoSwiss *does* publish its symbol table — it is linked
off the [weather symbols](https://www.meteoswiss.admin.ch/weather/weather-and-climate-from-a-to-z/weather-symbols.html)
page as "Weather symbols (XLSX)", dated 14 February 2022, and carries all 42 day
codes with German, French, Italian and English text. Codes ≥ 100 are the night
twins of the same weather. `condFromIcon()` is transcribed from that file, so it
is authoritative rather than inferred.

This matters most for thunderstorms: codes **12, 13, 23, 24, 25 and 36–42** are
all thundery. An earlier version of this firmware derived the table empirically
by correlating icon codes against sunshine and precipitation, and got four of
them wrong — a thunderstorm shows neither unusual sunshine nor unusual rainfall
probability, so correlation cannot find it. Anything outside codes 1–42 still
falls back to the precipitation probability.

The conditions the screen can report are clear, mostly sunny, partly cloudy,
cloudy, overcast, fog, showers, rain, sleet, snow and thunderstorms, each with
its own pictogram.

## Case

`case/ruuvibeacon_case.scad` is a parametric **two-part snap-fit enclosure**
designed to be stuck to a mirror with double-sided tape: the back is a flat,
unbroken face and there are no screws or screw heads anywhere.

Finished size **57.2 × 79.8 × 14.0 mm**. Every board dimension it uses is
measured from Waveshare's own 2D drawing and STEP model — see
[`case/DIMENSIONS.md`](case/DIMENSIONS.md), which also records where each number
came from.

The board is a sandwich, not a bare PCB: the cover glass (50.54 × 73.06) is the
outermost body and it covers all four mounting holes, so nothing can be screwed
on from the front. The back shell instead supports the PCB on four posts whose
spigots locate it through the mounting holes, and the bezel retains the glass
from the front. The back shell is the structural part and carries the tape, so
the load path from mirror to board never runs through a snap.

```bash
openscad -D 'part="shell"' -o ruuvibeacon_shell.stl case/ruuvibeacon_case.scad
openscad -D 'part="bezel"' -o ruuvibeacon_bezel.stl case/ruuvibeacon_case.scad
```

`part="fit"` renders both halves around a stand-in board for checking
clearances; `part="both"` (the default) lays the two parts out side by side.

**Printing.** PETG is the better choice — a bathroom mirror is a humid spot, and
the snap clips are tougher in PETG than PLA. 0.4 mm nozzle, 0.2 mm layers, 3
perimeters. Neither part needs supports: print the shell back-face-down and the
bezel front-face-down (which also puts the best surface on the visible side).
The clips peak at roughly 2% strain on assembly, which is within PETG's range.

**Assembly.** Drop the board in component-side-down so the four spigots pick up
the mounting holes, then press the bezel on until the four clips click. To open
it again, lever at the two notches on the top edge. If the board rattles, a
strip of 0.5 mm foam tape between bezel and glass takes up the slack — don't
overtighten anything against the LCD.

**Mounting.** Degrease the mirror with isopropyl first; VHB foam tape or Command
strips both work on the flat back. Use a **right-angle USB-C cable** — USB-C
exits the middle of a short edge, so a straight plug juts out sideways whereas a
right-angle one turns immediately and runs flush down the glass. The opening is
sized to clear a right-angle connector.

**The chime — read this before printing.** This case has **no speaker bay**, so
as drawn it drops the chime listed under Features. The bundled 2030 speaker is
20 × 30 mm; its thickness is not published and this class of part runs roughly
2.5–4 mm. Behind the PCB there is 4.4 mm of space, already carrying components
up to 4.0 mm tall — but that 4.0 mm is a *maximum over the whole board*, not a
uniform slab, so whether the speaker fits depends on there being a clear region
big enough for it. The drawing does not say where the board is clear, so this
isn't something to settle without the board in hand. If you want the chime:
measure your speaker and the clear area behind the board, then raise
`comp_clear` to buy depth. The **−X edge is the natural place for sound slots** —
it carries the speaker JST and has no openings or clips on it.

**Contrast on the real panel.** The design puts hairlines at 32% ink, already
raised from 16% on the grounds that "a lighter 1 px rule disappears". On the
actual ST7789 that reasoning goes further — 32% (2.0:1) still washed out, so
the rules run at **60%** (4.4:1), the histogram bar darkened to `#35597a`
(5.1:1 against its track, where the design's `#5980a6` managed only 2.8:1), and
the cloud fill went a step darker. Everything else keeps the design's tokens.

**A gotcha worth not reintroducing.** MeteoSwiss sends
`precipitationProbability3h` as JSON *floats* (`65.0`, not `65`), and
ArduinoJson's `|` operator returns the default unless `is<T>()` matches —
`is<int>()` is false for a float. Reading these with an int default
(`p3h[i] | 0`) silently collapses **every** probability to zero while the
temperatures, which use a float default, come through fine. Read them as float
and round.

**Things you may want to change** (all variables at the top of the `.scad`):

- `want_btns` / `want_tf` — set `false` for an unbroken side edge if you would
  rather configure the device before mounting it and never touch the buttons or
  card slot again.
- `glass_fit`, `snap_gap`, `hook_eng` — fit and snap tuning if your printer runs
  tight or loose.
- The side openings deliberately span the full depth available to a back-side
  component rather than being fitted to actuator heights the drawing never
  gives, so they cannot miss. Tighten `btn_z0`/`tf_z0` if you prefer neater
  slots and have measured your board.

## Restore the factory demo

The original Waveshare firmware was backed up before flashing:

```bash
esptool write-flash 0x0 waveshare-factory-backup.bin
```
