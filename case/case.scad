// ESP-ICU case: XIAO ESP32-S3 Sense (camera + expansion board), minimal - acidvegas
// Frame: origin = base board center, +X toward USB-C, Z-up, z=0 = bottom of base PCB (see board_stl.py)
// Sources: Seeed Sense STEP, Sense KiCad v1.51, expansion board DXF (ref/)

part = "both"; // tray | lid | both (print layout)

$fn = 64;

// Base board
bw = 20.955;  // PCB X
bh = 17.78;   // PCB Y
br = 1.905;   // PCB corner radius
bt = 1.25;    // PCB top (1.2 board + 0.05 bottom pads)
usb_x = 12.0; // USB-C receptacle face
usb_h = 4.46;
usb_w = 8.94;

// Stack (from the STEP)
exp_x0  = -10.78;              // expansion board / FPC connector overhang at -X
cam_top = 10.30;               // top of the camera's square holder
lens_top = 13.96;              // front of the lens barrel
cam_x1  = 10.95;               // camera holder +X edge (past the USB-end wall inner face)
cam_y   = [-3.36, 4.64];
lens    = [6.95, 0.64];        // lens axis
lens_d  = 7.85;                // widest barrel
sd_y    = [-4.77, 6.41];       // microSD card, loads from the USB end
sd_z    = [6.85, 8.50];
fpc_top = 7.43;                // 24-pin FPC connector top (clamp surface)

// SMA bulkhead (generic U.FL -> SMA female pigtail: 1/4"-36 thread, 8 mm hex nut, ~8-9 mm body behind the panel)
sma_ext  = 11;   // antenna compartment length behind the panel (body + pigtail coil)
sma_wall = 2.0;  // panel thickness at the SMA
sma_d    = 6.6;  // round panel hole, takes both D-flat and plain threads

// Heatsinks on the back of the base board: two side by side, 9.3 x 20.2 total, 1 mm from the USB end,
// 4.7 / 3.6 mm from the long edges. Floor cutout spans the full length and covers either left/right reading
hs_y = 5.2 + 0.3;

// Case
gap   = 0.2;
wall  = 1.6;  // side walls (tongue 0.8 + lid skin 0.7 + 0.1 fit)
ewall = 1.2;  // USB-end wall (USB face flush)
skin  = 0.8;
fit   = 0.1;
floor = 0.8;
tab   = 0.9;  // BOOT flexure thickness
flush = 0.1;  // lens sits this far below the top face

z_in  = cam_top + 0.1;  // underside of roof
z_s   = bt + 0.05;      // tray wall top / split
z_t   = z_s + 1.5;      // tongue top
sma_z = (z_s + lens_top + flush - 1.0) / 2 + 1.5; // SMA axis, 1.5 above compartment center (toward the camera face)
z_top = lens_top + flush; // flat top, lens recessed
z_tab = z_top - tab;      // underside of the BOOT flexure

cav_x0 = exp_x0 - gap;
cav_x1 = usb_x - ewall;
cav_y  = bh/2 + gap;
cav_xe = cav_x0 - sma_ext; // inner face of the SMA end wall
cx0 = cav_xe - sma_wall;
cy  = cav_y + wall;
cr  = 3.4;

// Features
boot    = [8.89, -5.92]; btn_top = 1.98; // K2 = GPIO0 (BOOT). K1 = RESET sits under the SD card, not reachable
mic     = [-2.75, 7.31];                 // expansion-board PDM mic port (from DXF)

module rrect(x0, x1, y0, y1, r) { translate([x0 + r, y0 + r]) offset(r) square([x1 - x0 - 2*r, y1 - y0 - 2*r]); }
module outer2d()     { rrect(cx0, usb_x, -cy, cy, cr); }
module board2d()     { offset(gap) rrect(-bw/2 - 0.4, bw/2, -bh/2, bh/2, br); } // +0.4 slack at the SMA end
module stack2d()     { rrect(cav_xe, cav_x1, -cav_y, cav_y, 2.1); }
module slab(z0, z1)  { translate([0, 0, z0]) linear_extrude(z1 - z0) children(); }
module box(x0, x1, y0, y1, z0, z1) { translate([x0, y0, z0]) cube([x1 - x0, y1 - y0, z1 - z0]); }
module no_end(d = 0) { translate([cx0 - 1, -cy - 1]) square([cav_x1 - 0.4 + d - cx0 + 1, 2*cy + 2]); } // keeps the tongue off the USB-end wall

// Snap: ridge on the tray tongue's inner face clicks into a groove in the lid lip (both long sides)
snap_x = [-9, 1];
module snap2d(pts, e = 0) { for (m = [0, 1]) mirror([0, m, 0]) translate([snap_x[0] - e, 0, 0]) rotate([90, 0, 90]) linear_extrude(snap_x[1] - snap_x[0] + 2*e) polygon(pts); }
module ridge()  { yi = cy - skin;       snap2d([[yi + 0.01, 1.7], [yi - 0.3, 2.1], [yi + 0.01, 2.5]]); }
module groove() { yo = cy - skin - fit; snap2d([[yo + 0.1, 1.55], [yo - 0.4, 2.1], [yo + 0.1, 2.65]], 0.5); }

module openings() {
	box(cav_x1 - 1, usb_x + 1, -usb_w/2 - 0.25, usb_w/2 + 0.25, 0.1, usb_h + 0.2);            // USB-C
	box(cav_x1 - 1, usb_x + 1, sd_y[0] - 0.25, sd_y[1] + 0.25, sd_z[0] - 0.25, sd_z[1] + 0.2); // microSD
	box(cav_x1 - 1, cam_x1 + 0.2, cam_y[0] - 0.2, cam_y[1] + 0.2, z_s, z_in);                  // camera holder pocket (wall 0.85 here)
}

module tray() {
	difference() {
		union() {
			slab(-floor, z_s) outer2d();
			slab(z_s, z_t) intersection() { difference() { outer2d(); offset(delta = -skin) outer2d(); } no_end(); }
		}
		slab(0, z_s + 1) board2d();
		box(-bw/2 - gap, cav_x1, -hs_y, hs_y, -floor - 1, 0.01); // heatsink cutout
		slab(z_s, z_t + 1) stack2d();
		openings();
	}
	ridge();
}

module lid() {
	difference() {
		union() {
			slab(z_t, z_top) outer2d();
			slab(z_s + fit, z_t) difference() {
				outer2d();
				intersection() { difference() { outer2d(); offset(delta = -(skin + fit)) outer2d(); } no_end(fit); }
			}
		}
		slab(z_s - 1, z_in) stack2d();
		box(cav_xe, cav_x0, -cav_y + 1, cav_y - 1, z_in - 1, z_top - 1.0); // taller antenna compartment
		translate([cx0 - 1, 0, sma_z]) rotate([0, 90, 0]) cylinder(d = sma_d, h = sma_wall + 2);  // SMA panel hole
		openings();
		// lens bore stepped to the barrel profile
		translate([lens.x, lens.y, z_in - 1]) cylinder(d = lens_d + 0.4, h = 12.6 - z_in + 1);
		translate([lens.x, lens.y, z_in - 1]) cylinder(d = 6.8, h = 13.85 - z_in + 1);
		translate([lens.x, lens.y, z_in - 1]) cylinder(d = 4.8, h = z_top - z_in + 2);
		// thin the roof under the BOOT flexure
		slab(z_in - 1, z_tab) translate([boot.x - 6.8, boot.y - 2]) square([8.6, 4]);
		// BOOT flexure: U-slot around a tab hinged toward -X
		slab(z_in - 1, z_top + 1) difference() {
			translate([boot.x - 6.8, boot.y - 2]) square([8.6, 4]);
			translate([boot.x - 7.3, boot.y - 1.5]) square([8.6, 3]);
		}
		translate([mic.x, mic.y, z_in - 1]) cylinder(d = 1.5, h = z_top - z_in + 2);
		groove();
	}
	// BOOT plunger
	translate([boot.x, boot.y, btn_top + 0.25]) cylinder(d = 1.8, h = z_tab - btn_top - 0.25 + 0.01);
	// clamp posts on the FPC connector body, hold the whole stack down
	for (y = [-6, 6]) translate([-8.4, y, fpc_top + 0.05]) cylinder(d = 2, h = z_in - fpc_top - 0.05 + 0.01);
}

if (part == "tray") tray();
if (part == "lid")  lid();
if (part == "both") { tray(); translate([0, 2*cy + 4, z_top]) rotate([180, 0, 0]) lid(); }
