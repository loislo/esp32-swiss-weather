# ESP32-S3-Touch-LCD-2.8 — measured dimensions

Every number the case model uses, and where it came from. Nothing here is
estimated. Sources are Waveshare's own published files:

- 2D drawing — <https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8/ESP32-S3-Touch-LCD-2.8%202D.pdf>
- STEP model — <https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8/ESP32-S3-Touch-LCD-2.8_3D.zip>
- Interface photo — `ESP32-S3-Touch-LCD-2.8-introduction-02.png` on the wiki

## The thing that matters most

**The cover glass, not the PCB, is the outermost body.** The glass is
50.54 × 73.06; the PCB behind it is only 49.90 × 69.00 and sits concentric,
so the glass overhangs the PCB by ~0.32 mm on the long sides and 2.03 mm at
each end. Consequences:

- The glass **completely covers all four mounting holes**. Nothing can be
  screwed to the board from the front. The case has to hold the board as a
  sandwich — posts behind the PCB, bezel retaining the glass.
- Anything dimensioned around "the board" being 73.06 × 50.54 is wrong; that
  is the glass.

## Outline and screen

| Value | mm | Source |
|---|---|---|
| Cover glass / touch panel (TP OD) | 50.54 ±0.05 × 73.06 ±0.05 | drawing, front view |
| Glass corner radius | 4 × R2.00 | drawing |
| Touch active area (TP VA) | 43.80 ±0.15 × 58.20 ±0.15 | drawing |
| Display active area (LCD AA) | 43.20 × 57.60 | drawing |
| PCB | 49.90 × 69.00 × 1.60 | drawing, back view + STEP `BOARD` solid |

The screen is **not centred on the glass**. The drawing gives 3.37 mm of glass
either side of the VA on the short axis (so it *is* centred there: 50.54 −
43.80 = 6.74, half = 3.37), and 10.43 mm from the USB end of the glass to the
bottom of the VA. That puts the VA centre at 10.43 + 29.10 = 39.53 from the USB
end, versus 36.53 for the glass centre — an offset of exactly **+3.00 mm away
from the USB edge**.

The STEP model confirms this independently: the glass solid spans
Y −39.53 … +33.53, i.e. its centre is at −3.00, which means **the assembly
origin is the display active-area centre**.

## Z stack

The side view gives a chain of 10.00 / 6.00 / 3.60 (TP+LCD) / 0.70 (LENS),
all measured from the front glass surface. Resolving it:

| From front face | mm | What |
|---|---|---|
| 0.00 → 0.70 | 0.70 | cover lens |
| 0.00 → 3.60 | 3.60 | TP + LCD stack |
| 3.60 → 4.40 | 0.80 | gap (LCD mounting foam / FPC) |
| 4.40 → 6.00 | 1.60 | PCB |
| 6.00 → 10.00 | 4.00 | tallest back-side components |

So **4.40 mm of glass stack sits above the PCB front face** and **4.00 mm of
components below its back face**, totalling the quoted 10.00 mm.

## Mounting holes

From the STEP `BOARD` solid — four cylinders of radius 2.15 at local
(4.50, 4.50), (4.50, 45.50), (64.50, 4.50), (64.50, 45.50):

- **Ø4.30** through-holes
- **60.00 mm** spacing along the long axis, **41.00 mm** across the short axis
- **4.50 mm** from each PCB edge (the drawing's 4.50 callouts agree)

Around each hole there is a bare annulus of about **Ø8.3** with no components
in it (measured off the drawing at 1200 dpi). The Ø6.5 support posts in the
model sit inside that keepout with room to spare.

## Connector and control positions

USB-C is on a short edge. The drawing's bottom-edge chain from the lower-left
hole is 9.00 + 11.6 = 20.60 to the connector centre; adding the 4.45 hole
margin gives 25.05 against a PCB half-width of 24.95, so **USB-C is centred**
on that edge to within 0.1 mm.

The buttons and TF slot are chained off the upper-left hole in the back view
(7.84, then 7.82, 7.82, then 10.86). Referred to the board centre, with +Y
pointing away from USB:

| Feature | Y (mm) |
|---|---|
| BOOT | +22.16 |
| RESET | +14.34 |
| BAT PWR | +6.52 |
| TF card slot centre | −4.34 |

**Handedness.** Waveshare's interface photo is a view of the *back* with USB-C
at the bottom, and it shows BOOT/RESET/BAT-PWR and the TF slot on the left,
with the speaker and battery JSTs on the right. Mirrored to a front view with
USB still at the bottom, the buttons and TF slot are therefore on the
**right-hand (+X) edge** — which is where the model puts them.

## Bundled speaker

The wiki lists a "2030 speaker 8Ω 2W" in the box — a 20 × 30 mm rectangular
speaker. **Its thickness is not published**, and parts of this type run roughly
2.5–4 mm; treat any figure here as unmeasured.

The model has no speaker bay. Behind the PCB there is 4.4 mm of depth carrying
components up to 4.00 mm tall, but that 4.00 is a maximum over the whole board
(the side view shows the tall items — USB-C, the FPC and 12-pin connectors, the
JSTs — are localised), not a uniform slab. So the question is whether a clear
20 × 30 region exists, and the drawing does not give per-region heights. That
needs the board in hand.

The −X edge is the place for sound slots if a speaker is fitted: it carries the
speaker JST and the case puts no openings or snap clips on it.
