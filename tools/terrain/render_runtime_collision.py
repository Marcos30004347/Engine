#!/usr/bin/env python3
"""Diagnostic plots of exported collision queries; no synthetic world imagery."""
import json,sys
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
root=Path(sys.argv[1]);v=json.loads((root/'validation.json').read_text());gpu=json.loads((root/'gpu-validation.json').read_text())
assert v['passed'] and v['collision']['bruteForceParity'] and gpu['collisionRevisionsMatch']
queries=json.loads((root/'collision-probes.json').read_text());wall=json.loads((root/'wall-probes.json').read_text())
mesh=json.loads((root/'frame-0-mesh.json').read_text())['mesh']
font=ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc',24);small=ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc',18)
im=Image.new('RGB',(1500,930),'#17232b');d=ImageDraw.Draw(im)
d.text((28,24),'STEP 17b.3 / terrain, water and structural segment collision',font=font,fill='#e5ebdf')
d.text((28,74),f"{v['collision']['triangles']} finest collision triangles / {v['collision']['nodes']} BVH nodes / 1,000 queries match brute force",font=small,fill='#c6d3cd')
d.text((28,127),'Top view: first 80 oblique probes',font=font,fill='#e5ebdf')
project=lambda p:(60+p[0]*9,205+p[2]*9)
for t,m in zip(mesh['triangles'],mesh['materials']):
    d.polygon([project(mesh['vertices'][i]) for i in t],fill={0:'#627e5e',1:'#b2aa82',9:'#4b8fab'}.get(m,'#aaa288'))
for q in queries:
    a=project(q['from']);b=project(q['point'] if q['hit'] else q['to']);col='#f2dfa4' if q['hit'] else '#f39b85'
    d.line((a,b),fill=col,width=2);d.ellipse((b[0]-3,b[1]-3,b[0]+3,b[1]+3),fill=col)
d.text((28,684),'Yellow: nearest hit   Pink: miss   Geometry projected into XZ.',font=small,fill='#c6d3cd')
d.text((800,127),'Vertical wall: 400 cross-section probes',font=font,fill='#e5ebdf')
for y,z,hit in wall:
    x=800+z*200;py=625-y*200
    d.rectangle((x-9,py-9,x+9,py+9),fill='#f2dfa4' if hit else '#3c5158')
d.line((800,625,800,225,1200,625,800,625),fill='#e5ebdf',width=2)
d.text((800,655),'Wall at x=0, y+z <= 2 m; horizontal rays from x=-2 to +2.',font=small,fill='#c6d3cd')
for i,(frame,rev,tris,collision_rev) in enumerate(gpu['rows']):
    x=28+i*245;d.rounded_rectangle((x,760,x+225,827),radius=7,fill='#2f4d54')
    d.text((x+10,769),f'Frame {frame}: R{rev} / C{collision_rev}',font=small,fill='#e5ebdf')
    d.text((x+10,796),'Render / collision revision',font=small,fill='#c6d3cd')
d.text((28,870),'Two-sided finite segments, original normals, finest geometry at every display LOD. No swept-body/contact solver.',font=small,fill='#c6d3cd')
im.save(root/'17-runtime-collision.png')
print('PASS collision diagnostic from actual triangle geometry, queries and Vulkan revision logs')
