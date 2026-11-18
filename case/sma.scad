// ESP-ICU: generic U.FL -> SMA female bulkhead, preview only - acidvegas
include <case.scad>
part = "none";
translate([cx0, 0, sma_z]) rotate([0, -90, 0]) {
	translate([0, 0, -9]) cylinder(d = 6.4, h = 9);           // body behind the panel
	translate([0, 0, -sma_wall]) cylinder(d = 6.35, h = 9.5); // 1/4"-36 thread
	cylinder(d = 9.2, h = 2, $fn = 6);                         // 8 mm hex nut
}
