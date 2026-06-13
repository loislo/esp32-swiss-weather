// =============================================================================
// RuuviBeacon case — Waveshare ESP32-S3-Touch-LCD-2.8 (no battery)
// Two-part friction-fit enclosure: back tray (cradles the PCB by its edges)
// + front bezel (window for the screen, skirt slips over the tray).
//
// Board outer size 73.06 x 50.54 mm is from Waveshare's docs. Everything else
// (component heights, screen-window position, USB-C location) is NOT published,
// so the values below are reasonable estimates. >>> MEASURE the ones marked
// (MEASURE) against your board with calipers and adjust before printing. <<<
//
// Render parts:  openscad -D 'part="back"'  -o back.stl  ruuvibeacon_case.scad
//                openscad -D 'part="bezel"' -o bezel.stl ruuvibeacon_case.scad
// part="both" (default) shows them side by side for preview.
// =============================================================================

part = "both";          // "back" | "bezel" | "both"
$fn = 48;

// ---- board ----
board_w   = 73.06;      // long edge (X)
board_h   = 50.54;      // short edge (Y)
pcb_th    = 1.6;        // PCB thickness
back_clear= 4.0;        // (MEASURE) clearance under PCB for back-side components
front_clear=3.2;        // (MEASURE) height the glass/touch panel stands above the PCB front

// ---- shell ----
wall      = 2.0;        // wall thickness
floor_th  = 1.6;        // back floor thickness
fit       = 0.4;        // gap around the PCB inside the tray
ledge_w   = 1.6;        // width of the rim the PCB rests on
corner_r  = 2.5;        // outer corner radius

// ---- bezel ----
bezel_th    = 2.0;      // thickness of the front face
bezel_border= 3.0;      // how much the bezel overlaps the PCB front edge (holds it down)
skirt_h     = 7.0;      // how far the bezel skirt slides over the tray
skirt_gap   = 0.30;     // friction-fit clearance between skirt and tray

// ---- screen window (MEASURE: active glass rectangle + its position) ----
// active area of a 2.8" 240x320 panel ~ 43.2 x 57.6 mm; +reveal for the bezel opening
screen_w     = 44.0;    // window size across the SHORT axis (Y)
screen_h     = 59.0;    // window size along the LONG axis (X)
screen_off_x = 0.0;     // shift window along X from board centre (+ toward USB edge)
screen_off_y = 0.0;     // shift window along Y from board centre

// ---- USB-C cutout (MEASURE: edge + offset). Default: +X short edge, centred ----
usb_w        = 9.6;     // cutout width
usb_h        = 4.2;     // cutout height
usb_off      = 0.0;     // offset along the edge from centre
usb_edge     = "x+";    // "x+","x-","y+","y-"  which edge the USB-C is on

// ---- derived ----
cav_w = board_w + 2*fit;
cav_h = board_h + 2*fit;
out_w = cav_w + 2*wall;
out_h = cav_h + 2*wall;
pcb_bottom_z = floor_th + back_clear;          // PCB rests here
pcb_top_z    = pcb_bottom_z + pcb_th;
tray_top_z   = pcb_top_z;                        // tray walls end flush with PCB front

// rounded box helper (flat-bottom prism with rounded vertical edges)
module rbox(w, h, z, r) {
  linear_extrude(z)
    offset(r=r) offset(r=-r) square([w, h]);
}

// ---------------------------------------------------------------- BACK TRAY
module back_shell() {
  difference() {
    rbox(out_w, out_h, tray_top_z, corner_r);                 // solid block
    // component + PCB cavity (hollow from the floor up to the top)
    translate([wall, wall, floor_th])
      cube([cav_w, cav_h, tray_top_z]);                       // full-height void
    usb_cutout();                                             // USB-C slot
  }
  // PCB rest ledge: a thin rim protruding inward; the PCB sits on its top face
  difference() {
    translate([wall, wall, pcb_bottom_z - ledge_h()])
      cube([cav_w, cav_h, ledge_h()]);
    translate([wall+ledge_w, wall+ledge_w, pcb_bottom_z - ledge_h() - 0.1])
      cube([cav_w-2*ledge_w, cav_h-2*ledge_w, ledge_h()+0.2]);
  }
}
function ledge_h() = min(2.0, back_clear);   // ledge height (don't exceed cavity)

module usb_cutout() {
  // a slot through the wall at PCB level, on the chosen edge
  z = pcb_bottom_z + pcb_th/2;
  ex = 4*wall;                                  // make sure it punches through
  if (usb_edge == "x+")
    translate([out_w - wall - 0.1, out_h/2 + usb_off - usb_w/2, z - usb_h/2])
      cube([wall+ex, usb_w, usb_h]);
  else if (usb_edge == "x-")
    translate([-ex+0.1, out_h/2 + usb_off - usb_w/2, z - usb_h/2])
      cube([wall+ex, usb_w, usb_h]);
  else if (usb_edge == "y+")
    translate([out_w/2 + usb_off - usb_w/2, out_h - wall - 0.1, z - usb_h/2])
      cube([usb_w, wall+ex, usb_h]);
  else // y-
    translate([out_w/2 + usb_off - usb_w/2, -ex+0.1, z - usb_h/2])
      cube([usb_w, wall+ex, usb_h]);
}

// ---------------------------------------------------------------- FRONT BEZEL
module front_bezel() {
  bz_out_w = out_w + 2*skirt_gap + 2*wall;
  bz_out_h = out_h + 2*skirt_gap + 2*wall;
  // front face plate sits on top of the tray (z = tray_top_z), window cut out
  translate([-(wall+skirt_gap), -(wall+skirt_gap), 0]) {
    difference() {
      union() {
        // top frame plate
        translate([0,0, tray_top_z])
          rbox(bz_out_w, bz_out_h, bezel_th, corner_r);
        // skirt going down over the tray outside
        difference() {
          translate([0,0, tray_top_z - skirt_h])
            rbox(bz_out_w, bz_out_h, skirt_h, corner_r);
          translate([wall, wall, tray_top_z - skirt_h - 0.1])
            rbox(bz_out_w-2*wall, bz_out_h-2*wall, skirt_h+0.2, max(0.1,corner_r-wall));
        }
      }
      // screen window through the plate (sized to the glass, minus bezel_border overlap)
      wx = screen_h;   // along X (long axis)
      wy = screen_w;   // along Y (short axis)
      cx = (wall+skirt_gap) + out_w/2 + screen_off_x;
      cy = (wall+skirt_gap) + out_h/2 + screen_off_y;
      translate([cx - wx/2, cy - wy/2, tray_top_z - 0.1])
        cube([wx, wy, bezel_th + 0.2]);
    }
  }
}

// ---------------------------------------------------------------- LAYOUT
if (part == "back")       back_shell();
else if (part == "bezel") front_bezel();
else {
  back_shell();
  translate([out_w + 12, 0, 0]) front_bezel();
}
