#!/usr/bin/env python3
# ESP-ICU: convert the official XIAO ESP32-S3 Sense STEP into the case frame - acidvegas
# Case frame: origin = board center, +X toward USB-C, Z-up, z=0 at the bottom of the PCB

import cadquery as cq
import numpy as np
import trimesh

STEP = 'ref/sense-3d/Seeed Studio XIAO-ESP32-S3-Sense.step'

a  = cq.importers.importStep(STEP)
pcb = max(a.solids().vals(), key=lambda s: s.Volume()).BoundingBox()
xc, zc, y0 = (pcb.xmin + pcb.xmax) / 2, (pcb.zmin + pcb.zmax) / 2, pcb.ymin
cq.exporters.export(a, 'stl/board_raw.stl', tolerance=0.02, angularTolerance=0.2)

m = trimesh.load('stl/board_raw.stl')
v = m.vertices.copy()
m.vertices = np.column_stack([v[:, 0] - xc, -(v[:, 2] - zc), v[:, 1] - y0])
m.export('stl/board.stl')
print('board bbox', np.round(m.bounds, 3).tolist())
