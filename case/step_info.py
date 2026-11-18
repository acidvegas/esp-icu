# ESP-ICU: list the solids in a STEP file with their bounding boxes - acidvegas
import sys, cadquery as cq
a = cq.importers.importStep(sys.argv[1])
bb = a.val().BoundingBox()
print('bbox', [round(x,2) for x in (bb.xmin,bb.xmax,bb.ymin,bb.ymax,bb.zmin,bb.zmax)])
rows=[]
for s in a.solids().vals():
    b=s.BoundingBox(); rows.append((round(s.Volume(),1),)+tuple(round(x,2) for x in (b.xmin,b.xmax,b.ymin,b.ymax,b.zmin,b.zmax)))
print('solids', len(rows))
for r in sorted(rows, reverse=True)[:int(sys.argv[2]) if len(sys.argv)>2 else 30]: print(r)
