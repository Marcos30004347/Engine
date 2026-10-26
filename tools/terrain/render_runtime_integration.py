#!/usr/bin/env python3
"""Render verified runtime logs and native mesh captures as an acceptance sheet."""
from pathlib import Path
import json, re, sys
from PIL import Image, ImageDraw, ImageFont
root=Path(sys.argv[1])
validation=json.loads((root/'validation.json').read_text())
gpu=json.loads((root/'gpu-validation.json').read_text())
assert validation['passed'] and gpu['passed']
rows=re.findall(r'PASS rewrite GPU frame=(\d+) revision=(\d+) triangles=(\d+) budget-rejection-preserved-parent=1',(root/'viewer.log').read_text())
assert len(rows)==gpu['replacements']==6
font=ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc',24)
small=ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc',18)
im=Image.new('RGB',(1500,940),'#17232b');d=ImageDraw.Draw(im)
d.text((28,25),'STEP 17 / compiled world archive, checked GPU replacement and edit rebuilding',font=font,fill='#e4eade')
d.text((28,77),f"{validation['assertions']} CPU assertions | 60 live Vulkan frames | 6 replacements + 6 preserved-parent rejections",font=small,fill='#c8d4cc')
for i,(frame,revision,triangles) in enumerate(rows):
    x=28+(i%3)*495;y=135+(i//3)*135
    d.rounded_rectangle((x,y,x+465,y+105),radius=10,fill='#2f4e55')
    d.text((x+15,y+16),f'Frame {frame} / world revision {revision}',font=font,fill='#e4eade')
    d.text((x+15,y+59),f'{triangles} triangles / failed upload retained parent',font=small,fill='#c8d4cc')
d.text((28,427),'Same compiled geometry, rendered by the native Metal capture tool:',font=small,fill='#c8d4cc')
for lod in range(3):
    capture=Image.open(root/f'lod-{lod}-oblique.png');capture.thumbnail((460,330))
    x=28+lod*495;im.paste(capture,(x,475))
    d.text((x,803),f"LOD {lod}: {validation['lods'][lod]['triangles']} triangles",font=small,fill='#e4eade')
d.text((28,863),'Bounded fixture. Full-world producers, cover mip rendering and Step 00 comparison remain unqualified.',font=small,fill='#e4eade')
d.text((28,898),'These captures are not screenshots of the Vulkan viewer; its actual playback is recorded in viewer.log.',font=small,fill='#c8d4cc')
im.save(root/'17-runtime-integration.png')
print('PASS runtime integration review sheet from verified logs and native meshes')
