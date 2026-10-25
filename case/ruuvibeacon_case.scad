// =============================================================================
// RuuviBeacon case — Waveshare ESP32-S3-Touch-LCD-2.8
//
// Two-part snap-fit enclosure intended to be stuck to a mirror with
// double-sided tape: the back of the shell is a flat, uninterrupted face.
//
// The board is a SANDWICH, not a bare PCB. The cover glass (50.54 x 73.06) is
// the outermost body and it fully covers the PCB (49.90 x 69.00) behind it,
// including all four mounting holes — so nothing can be screwed from the front.
// The shell therefore supports the PCB on four posts and the bezel retains the
// glass from the front; the two shells snap together around the glass.
//
// The BACK SHELL is the structural part and carries the tape, so the load path
// from mirror to board never passes through a snap.
//
// Board numbers below are measured from Waveshare's own 2D drawing and STEP
// model, not estimated — see case/DIMENSIONS.md. Don't change them.
//
// Render:
//   openscad -D 'part="shell"' -o ruuvibeacon_shell.stl case/ruuvibeacon_case.scad
//   openscad -D 'part="bezel"' -o ruuvibeacon_bezel.stl case/ruuvibeacon_case.scad
// part="both" (default) previews them side by side.
// part="fit" renders shell+bezel+board together to eyeball clearances.
//
// Orientation of this model: looking at the SCREEN, +X right, +Y up, +Z toward
// you. The back face (against the mirror) is Z=0. USB-C is on the -Y edge;
// BOOT/RESET/BAT-PWR and the TF slot are on the +X edge. The firmware runs the
// display in landscape, so in use the case is rotated 90deg and the -Y (USB)
// edge becomes a vertical side.
// =============================================================================

part = "both";          // "shell" | "bezel" | "both" | "fit"
$fn  = 64;

// ---------------------------------------------------------------- BOARD (measured)
glass_w   = 50.54;      // cover glass / touch panel, short axis (X)
glass_h   = 73.06;      // cover glass, long axis (Y)
glass_r   = 2.00;       // glass corner radius
glass_th  = 3.60;       // TP + LCD stack thickness
pcb_w     = 49.90;
pcb_h     = 69.00;
pcb_th    = 1.60;
back_comp = 4.00;       // tallest component behind the PCB
lcd_gap   = 0.80;       // air gap, LCD back face to PCB front face
aa_w      = 43.20;      // display active area
aa_h      = 57.60;
aa_off    = 3.00;       // AA centre offset from glass centre, away from USB (+Y)
hole_dx   = 41.00;      // mounting-hole spacing across the short axis (X)
hole_dy   = 60.00;      // mounting-hole spacing along the long axis (Y)
hole_d    = 4.30;       // mounting hole diameter

// ---------------------------------------------------------------- FIT / TUNING
glass_fit  = 0.35;      // clearance around the glass inside the shell
tower_wall = 1.60;      // shell wall alongside the glass
snap_gap   = 0.20;      // sliding clearance between tower and skirt
skirt_th   = 1.20;      // bezel skirt wall (also the snap cantilever thickness)
back_th    = 1.60;      // back plate — the face that takes the tape
comp_clear = 0.40;      // headroom over the tallest back-side component
bezel_th   = 2.00;      // bezel front plate
flange_h   = 3.00;      // height of the full-width shell flange (the parting line)
rim_gap    = 0.30;      // tower rim sits this far below the glass front, so the
                        // bezel lands on the GLASS and not on the shell rim
post_d     = 6.50;      // PCB support post
keepout_d  = 8.00;      // bare annulus around each mounting hole (measured 8.3)
spigot_d   = 4.10;      // locating spigot, into the 4.30 mounting hole
edge_cham  = 0.40;      // chamfer on the back outer edge (print release)
face_cham  = 0.80;      // chamfer on the front outer edge (looks)
win_cham   = 1.00;      // flare on the screen window

// ---------------------------------------------------------------- OPENINGS
usb_w      = 13.00;     // USB-C, on the -Y edge, centred in X
usb_z0     = 1.00;      // deliberately below back_th: locally thins the back
usb_z1     = 7.20;      //   plate to 1.0 mm so a chunky overmould clears
// The drawing does not dimension actuator or socket heights, so rather than
// fit these to a guess, each opening spans the FULL depth available to a
// back-side component (floor at back_th up to the PCB back face at pcb_z0).
// Anything mounted back-side therefore lands inside its opening whatever its
// height. This costs nothing but a slightly taller slot.
btn_w      = 4.50;      // BOOT / RESET / BAT-PWR, on the +X edge
btn_ys     = [22.16, 14.34, 6.52];
btn_z0     = 1.90;      // just above the floor
btn_z1     = 6.40;      // just past the PCB back face
tf_y       = -4.34;     // TF card slot, on the +X edge
tf_w       = 14.00;
tf_z0      = 3.60;
tf_z1      = 6.40;
want_btns  = true;      // set false for an unbroken +X edge
want_tf    = true;

// ---------------------------------------------------------------- SNAP
clip_ys    = [30.00, -30.00];   // two per long (+/-X) side
clip_w     = 9.00;
hook_eng   = 0.60;      // how far the hook reaches inward
hook_top   = 4.60;      // retention face height
tongue_top = 10.80;     // slots stop here, so the cantilever is ~7.5 mm long
slot_w     = 0.80;

// ---------------------------------------------------------------- DERIVED
cav_w   = glass_w + 2*glass_fit;          // 51.24
cav_h   = glass_h + 2*glass_fit;          // 73.76
cav_r   = glass_r + glass_fit;
tow_w   = cav_w + 2*tower_wall;           // 54.44
tow_h   = cav_h + 2*tower_wall;
tow_r   = cav_r + tower_wall;
out_w   = tow_w + 2*snap_gap + 2*skirt_th;   // 57.24
out_h   = tow_h + 2*snap_gap + 2*skirt_th;   // 79.76
out_r   = tow_r + snap_gap + skirt_th;
skin_w  = tow_w + 2*snap_gap;             // skirt inner
skin_h  = tow_h + 2*snap_gap;
skin_r  = tow_r + snap_gap;

pcb_z0   = back_th + comp_clear + back_comp;   // 6.00  PCB back face
pcb_z1   = pcb_z0 + pcb_th;                    // 7.60
glass_z0 = pcb_z1 + lcd_gap;                   // 8.40
glass_z1 = glass_z0 + glass_th;                // 12.00
tow_top  = glass_z1 - rim_gap;                 // 11.70
top_z    = glass_z1 + bezel_th;                // 14.00

win_w = aa_w + 0.8;     // 44.00 — reveals the whole active area plus a hair
win_h = aa_h + 0.8;     // 58.40
win_r = 3.00;

eps = 0.01;

// ---------------------------------------------------------------- HELPERS
module rrect(w, h, r) { offset(r=r) square([w-2*r, h-2*r], center=true); }
module prism(w, h, r, z0, z1) {
  translate([0, 0, z0]) linear_extrude(z1-z0) rrect(w, h, r);
}
// prism with the top outer edge chamfered by c
module prism_ctop(w, h, r, z0, z1, c) {
  hull() {
    prism(w, h, r, z0, z1-c);
    prism(w-2*c, h-2*c, max(r-c, 0.1), z1-eps, z1);
  }
}
// prism with the bottom outer edge chamfered by c
module prism_cbot(w, h, r, z0, z1, c) {
  hull() {
    prism(w-2*c, h-2*c, max(r-c, 0.1), z0, z0+eps);
    prism(w, h, r, z0+c, z1);
  }
}

// ---------------------------------------------------------------- OPENINGS
// Cut from BOTH parts; each keeps only what falls in its own Z range.
module openings() {
  // USB-C, -Y edge, centred. Reaches inboard past the PCB edge so a plug nose
  // can seat, and in doing so thins the back plate locally to usb_z0.
  translate([-usb_w/2, -(out_h/2 + 1), usb_z0])
    cube([usb_w, (out_h/2 + 1) - (pcb_h/2 - 1.5), usb_z1-usb_z0]);
  // side buttons, +X edge
  if (want_btns)
    for (y = btn_ys)
      translate([cav_w/2 - 1, y - btn_w/2, btn_z0])
        cube([out_w, btn_w, btn_z1-btn_z0]);
  // TF card slot, +X edge
  if (want_tf)
    translate([cav_w/2 - 1, tf_y - tf_w/2, tf_z0])
      cube([out_w, tf_w, tf_z1-tf_z0]);
}

// ---------------------------------------------------------------- BACK SHELL
module posts() {
  for (sx = [-1, 1], sy = [-1, 1])
    translate([sx*hole_dx/2, sy*hole_dy/2, 0]) {
      cylinder(d=post_d,   h=pcb_z0 - back_th + eps);   // support shoulder
      cylinder(d=spigot_d, h=pcb_z0 - back_th + pcb_th); // locating spigot
    }
}

module clip_pockets() {
  for (sx = [-1, 1], y = clip_ys)
    translate([sx*(tow_w/2 - 0.40), y, (flange_h + hook_top)/2])
      cube([0.80 + eps, clip_w + 0.4, hook_top - flange_h + 0.2], center=true);
}

// a nail-sized gap under the skirt on the +Y end, to pop the bezel off
module pry_notches() {
  for (x = [-12, 12])
    translate([x, out_h/2 - 0.5, 2.45])
      cube([10, 2.0, 1.1], center=true);
}

module shell() {
  difference() {
    union() {
      prism_cbot(out_w, out_h, out_r, 0, flange_h, edge_cham);  // flange
      prism(tow_w, tow_h, tow_r, flange_h, tow_top);            // tower
    }
    prism(cav_w, cav_h, cav_r, back_th, tow_top + eps);         // cavity
    openings();
    clip_pockets();
    pry_notches();
  }
  // posts live inside the cavity, so add them after the difference
  translate([0, 0, back_th]) posts();
}

// ---------------------------------------------------------------- FRONT BEZEL
// hook profile in XZ, extruded along Y. Ramp underneath so it prints without
// support and rides over the tower on assembly; flat top face retains.
module hook(sx) {
  si = sx * skin_w/2;
  rotate([90, 0, 0])
    linear_extrude(height=clip_w, center=true)
      polygon([[si, flange_h],
               [si - sx*hook_eng, flange_h + 1.2],
               [si - sx*hook_eng, hook_top],
               [si, hook_top]]);
}

module bezel() {
  difference() {
    union() {
      prism_ctop(out_w, out_h, out_r, glass_z1, top_z, face_cham);  // front plate
      difference() {                                                // skirt
        prism(out_w, out_h, out_r, flange_h, glass_z1);
        prism(skin_w, skin_h, skin_r, flange_h - eps, glass_z1 + eps);
      }
      for (sx = [-1, 1], y = clip_ys) translate([0, y, 0]) hook(sx);
    }
    // screen window, flared toward the front face
    translate([0, aa_off, 0]) {
      prism(win_w, win_h, win_r, glass_z1 - eps, top_z - win_cham);
      hull() {
        prism(win_w, win_h, win_r, top_z - win_cham, top_z - win_cham + eps);
        prism(win_w + 2*win_cham, win_h + 2*win_cham, win_r + win_cham,
              top_z - eps, top_z);
      }
    }
    // free the cantilevers
    for (sx = [-1, 1], y = clip_ys, sy = [-1, 1])
      translate([sx*(out_w/2 - skirt_th/2 - 0.1),
                 y + sy*(clip_w/2 + slot_w/2),
                 (flange_h + tongue_top)/2])
        cube([skirt_th + 1, slot_w, tongue_top - flange_h], center=true);
    openings();
  }
}

// ---------------------------------------------------------------- BOARD (preview)
// Stand-in for the real board, used by the clearance check. The back-side
// components are modelled as a solid slab minus the bare annulus that
// Waveshare keeps clear around each mounting hole (measured Ø8.3 on the
// drawing; keepout_d is the conservative figure the posts must fit inside).
module board() {
  color("green") difference() {
    union() {
      translate([0,0,pcb_z0 - back_comp]) prism(pcb_w-2, pcb_h-2, 1, 0, back_comp);
      prism(pcb_w, pcb_h, 1, pcb_z0, pcb_z1);
    }
    for (sx=[-1,1], sy=[-1,1])
      translate([sx*hole_dx/2, sy*hole_dy/2, pcb_z0-1])
        cylinder(d=hole_d, h=pcb_th+2);
    for (sx=[-1,1], sy=[-1,1])                       // component keepout
      translate([sx*hole_dx/2, sy*hole_dy/2, pcb_z0 - back_comp - 1])
        cylinder(d=keepout_d, h=back_comp+1);
  }
  color("black", 0.45) prism(glass_w, glass_h, glass_r, glass_z0, glass_z1);
}

// ---------------------------------------------------------------- LAYOUT
if      (part == "shell") shell();
else if (part == "bezel") bezel();
else if (part == "fit")   { shell(); bezel(); board(); }
else { shell(); translate([out_w + 10, 0, 0]) bezel(); }
