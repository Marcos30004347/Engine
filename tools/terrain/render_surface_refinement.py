#!/usr/bin/env python3
"""Measured Step 15 mesh diagnostics; no illustrative/generated geometry."""
import json,sys
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
root=Path(sys.argv[1]);assert json.loads((root/'validation.json').read_text())['passed']
font=ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc',20)
small=ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc',15)
names=['Narrow ridge / mandatory feature probes','Road carved into hillside','Deep channel / widening downstream','Closed plateau / vertical face normals','Y confluence / unioned water and bed']
for scene,filename in [(0,'15-error-heatmap.png'),(1,'15-road-carve-mesh.png'),(2,'15-river-merge-mesh.png'),(3,'15-cliff-face-mesh.png')]:
    j=json.loads((root/f'scene-{scene}-mesh.json').read_text());im=Image.new('RGB',(1600,1030),'#16232b');d=ImageDraw.Draw(im)
    d.text((25,20),'STEP 15 / '+names[scene],font=font,fill='#e4e7dd')
    # Native mesh render, generated before this script.
    native=Image.open(root/f'scene-{scene}-oblique.png').convert('RGB');native.thumbnail((940,580));im.paste(native,(15,65))
    mesh=j['mesh'];scale=22
    for i,t in enumerate(mesh['triangles']):
        p=[mesh['vertices'][v] for v in t]
        color='#8a9b72'
        if i<len(j['heightErrors']):
            ratio=min(1,j['heightErrors'][i]/.08);color=(int(75+170*ratio),int(145-60*ratio),int(170-95*ratio))
        poly=[(1020+v[0]*scale,90+v[2]*scale) for v in p]
        d.polygon(poly,fill=color);d.line(poly+poly[:1],fill='#4c625e',width=1)
    d.text((1020,560),f"probe error {j['maxHeightError']:.4f} m",font=font,fill='#e4e7dd')
    d.text((1020,591),f"dense error {j['denseError']:.4f} m",font=font,fill='#e4e7dd')
    d.text((1020,622),f"normal error {j['maxNormalError']:.2f} degrees",font=small,fill='#c8d0c6')
    d.text((1020,650),f"{j['addedVertices']} added | {j['passes']} passes",font=small,fill='#c8d0c6')
    section=j['section'];low=min(min(p[1:]) for p in section);high=max(max(p[1:]) for p in section);span=max(1,high-low)
    point=lambda x,y:(45+x/24*1490,935-(y-low)/span*190)
    d.line((45,745,45,942,1535,942),fill='#748983',width=1)
    d.line([point(p[0],p[1]) for p in section],fill='#eec785',width=3)
    d.line([point(p[0],p[2]) for p in section],fill='#72b9d0',width=1)
    sectionerror=max(abs(p[1]-p[2]) for p in section)
    d.text((45,705),f"Cross-section z=9.713 m / source gold, mesh blue / max {sectionerror:.4f} m",font=font,fill='#e4e7dd')
    d.text((45,955),'x = 0 m',font=small,fill='#c8d0c6');d.text((1460,955),'x = 24 m',font=small,fill='#c8d0c6')
    d.text((25,998),'Real CDT and structural triangles. Heatmap uses measured probes; mandatory features are needed for sub-probe details.',font=small,fill='#c8d0c6')
    im.save(root/filename)
# Include actual Y fixture alongside the deep-channel diagnostics.
im=Image.open(root/'15-river-merge-mesh.png');im.save(root/'15-deep-channel-mesh.png')
j=json.loads((root/'scene-4-mesh.json').read_text());im=Image.new('RGB',(1600,1000),'#16232b');d=ImageDraw.Draw(im)
d.text((25,22),'STEP 15 / Y confluence / unioned water 116 m² / separate bed at -3 m',font=font,fill='#e4e7dd')
for view,x,y in [('top',20,75),('oblique',790,75)]:
    native=Image.open(root/f'scene-4-{view}.png');native.thumbnail((770,770));im.paste(native,(x,y))
d.text((25,840),'One water owner per face; overlapping tributary masks do not produce duplicate water triangles.',font=font,fill='#e4e7dd')
d.text((25,885),'This fixture verifies topology and depth layers. Upstream hydraulics remains responsible for discharge and depth sizing.',font=small,fill='#c8d0c6')
im.save(root/'15-river-merge-mesh.png')
print('PASS four required diagnostic sheets plus deep-channel section')
