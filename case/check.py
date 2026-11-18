#!/usr/bin/env python3
# ESP-ICU: render the case and check it against the Seeed STEP stack - acidvegas

import subprocess
import cadquery as cq
import numpy as np
import trimesh

STEP = 'ref/sense-3d/Seeed Studio XIAO-ESP32-S3-Sense.step'

for p in ('tray', 'lid'):
	subprocess.run(['openscad', '-q', '-D', f'part="{p}"', '-o', f'stl/{p}.stl', 'case.scad'], check=True)
subprocess.run(['openscad', '-q', '-D', 'part="both"', '-o', 'stl/print_plate.stl', 'case.scad'], check=True)

t, l = trimesh.load('stl/tray.stl'), trimesh.load('stl/lid.stl')
c = (t + l).bounds
print('watertight', t.is_watertight, l.is_watertight, '| outer', np.round(c[1] - c[0], 2).tolist(), '| z', np.round(c[:, 2], 2).tolist())
print('tray∩lid', round(trimesh.boolean.intersection([t, l], engine='manifold').volume, 4))

sol = cq.importers.importStep(STEP).solids().vals()
pcb = max(sol, key=lambda s: s.Volume()).BoundingBox()
xc, zc, y0 = (pcb.xmin + pcb.xmax) / 2, (pcb.zmin + pcb.zmax) / 2, pcb.ymin
ms = []
for s in sol:
	vs, fs = s.tessellate(0.02, 0.2)
	m = trimesh.Trimesh(np.array([[p.x - xc, -(p.z - zc), p.y - y0] for p in vs]), np.array(fs))
	if m.is_volume:
		ms.append((s.Volume(), m))
for n, part in (('tray', t), ('lid', l)):
	hits = [(round(v, 2), np.round(m.bounds, 1).tolist()) for v, m in ms if trimesh.boolean.intersection([m, part], engine='manifold').volume > 1e-3]
	print(n, 'hits', hits or 'none')
bad = []
for dz in np.arange(0.25, 16, 0.25):
	L = l.copy(); L.apply_translation([0, 0, dz])
	for v, m in ms:
		if not 135 < v < 136.5 and trimesh.boolean.intersection([m, L], engine='manifold').volume > 1e-3: # SD card out for assembly
			bad.append(dz); break
print('lid slide-down collisions', bad[:5] or 'none')
