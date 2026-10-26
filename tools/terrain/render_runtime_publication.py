#!/usr/bin/env python3
"""Bounded Step 17a LOD/publication diagnostics; no full-world qualification claims."""
from pathlib import Path
import json,sys
from PIL import Image,ImageDraw,ImageFont
root=Path(sys.argv[1]);validation=json.loads((root/'validation.json').read_text());assert validation['passed']
frames=[json.loads((root/f'frame-{i}-mesh.json').read_text()) for i in range(6)]
font=ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc',21);small=ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc',15)
colors=['#c9ad7a','#91ad82','#698a9b']
im=Image.new('RGB',(1600,980),'#17232b');d=ImageDraw.Draw(im)
d.text((25,22),'STEP 17a / unequal interior LODs / identical constrained seam stations',font=font,fill='#e4e7de')
for frame,x,label in [(0,25,'Complete coarse parent'),(2,820,'Complete mixed replacement')]:
    j=frames[frame];mesh=j['mesh'];scale=10
    for i,t in enumerate(mesh['triangles']):
        polygon=[(x+mesh['vertices'][v][0]*scale,110+mesh['vertices'][v][2]*scale) for v in t];tile=j['tileIds'][i]
        d.polygon(polygon,fill=colors[j['tileLevels'][tile]]);d.line(polygon+polygon[:1],fill='#3d5852',width=1)
    for sx in (16,32,48):d.line((x+sx*scale,110,x+sx*scale,590),fill='#f6d6bc',width=2)
    d.line((x,350,x+640,350),fill='#f6d6bc',width=2)
    d.text((x,630),label,font=font,fill='#e4e7de')
    d.text((x,668),f"{len(mesh['triangles'])} selected triangles | 8 tiles",font=small,fill='#c4cfc3')
d.text((25,860),'LOD 0: 5 cm    LOD 1: 15 cm    LOD 2: 40 cm measured-probe budgets. Collision remains at LOD 0.',font=small,fill='#c4cfc3')
d.text((25,915),'All source and tile edges remain unchanged; density changes only in patch interiors. No skirts conceal cracks.',font=small,fill='#c4cfc3');im.save(root/'17-lod-seams.png')
im=Image.new('RGB',(1600,870),'#17232b');d=ImageDraw.Draw(im)
d.text((25,22),'STEP 17a / atomic reader snapshots / parent stays visible until replacement is complete',font=font,fill='#e4e7de')
labels=['Coarse complete','2 of 8 jobs ready','Mixed complete','Isolated snapshot','Pinned old reader','Pinned new reader']
for n,j in enumerate(frames):
    x=25+n%3*530;y=95+n//3*360
    for tile,lod in enumerate(j['tileLevels']):
        tx=x+tile%4*115;ty=y+tile//4*90;d.rectangle((tx,ty,tx+110,ty+85),fill=colors[lod]);d.text((tx+12,ty+20),f"LOD {lod}",font=font,fill='#17232b')
    d.text((x,y+198),labels[n],font=font,fill='#e4e7de');d.text((x,y+230),f"revision {j['revision']} / ticket {j['ticket']}",font=small,fill='#c4cfc3')
d.text((25,820),'Geometry, collision, water and versioned cover pages share one immutable snapshot. These are CPU publication states.',font=small,fill='#c4cfc3');im.save(root/'17-runtime-version-map.png')
im=Image.new('RGB',(1800,1040),'#17232b');d=ImageDraw.Draw(im)
d.text((25,20),'STEP 17a / orbit-camera capture sequence / bounded relief, road and water fixture',font=font,fill='#e4e7de')
for n,j in enumerate(frames):
    native=Image.open(root/f'frame-{n}-oblique.png');native.thumbnail((570,360));x=25+n%3*595;y=85+n//3*450;im.paste(native,(x,y));d.text((x,y+335),labels[n],font=font,fill='#e4e7de');d.text((x,y+370),f"azimuth {j['cameraAzimuthDegrees']} degrees",font=small,fill='#c4cfc3')
d.text((25,995),'Actual native mesh captures. Full-world cities/farms/ranges, GPU upload fencing and main-viewer rollout are not yet qualified.',font=small,fill='#c4cfc3');im.save(root/'17-camera-sequence.png')
print('PASS three bounded publication/LOD review sheets')
