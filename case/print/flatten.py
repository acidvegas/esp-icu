#!/usr/bin/env python3
# Flatten OrcaSlicer BBL system profiles (resolve "inherits") for CLI slicing - acidvegas

import json
import os
import sys

BBL = os.path.expanduser('~/.local/opt/orcaslicer/squashfs-root/resources/profiles/BBL')


def load(kind: str, name: str) -> dict:
	'''Load a profile and merge its inherits chain, parent first.'''

	with open(os.path.join(BBL, kind, name + '.json')) as f:
		p = json.load(f)
	base = load(kind, p['inherits']) if p.get('inherits') else {}
	base.update(p)
	base.pop('inherits', None)
	return base


for kind, name, out in [a.split('|') for a in sys.argv[1:]]:
	p = load(kind, name)
	with open(out, 'w') as f:
		json.dump(p, f, indent=1)
	print(out, {k: p.get(k) for k in ('layer_height', 'nozzle_temperature', 'hot_plate_temp', 'textured_plate_temp', 'wall_loops', 'printer_model') if k in p})
