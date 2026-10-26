#!/usr/bin/env python3
"""Review actual semantic mip samples and viewer playback records."""
import json,sys
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
root=Path(sys.argv[1]);levels=json.loads((root/'cover-mip-probes.json').read_text());gpu=json.loads((root/'gpu-validation.json').read_text())
assert gpu['passed'] and len(levels)==3
font=ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc',24);small=ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc',18)
im=Image.new('RGB',(1500,890),'#17232b');d=ImageDraw.Draw(im)
d.text((28,25),'STEP 17b.4 / semantic cover mips / hard boundaries retained',font=font,fill='#e7eddf')
d.text((28,72),'Actual runtime reader samples. Noise contrast is amplified for inspection; gold marks the road owner.',font=small,fill='#c6d3cd')
for i,level in enumerate(levels):
    tile=Image.new('RGB',(level['width'],level['height']))
    for n,(owner,variation,weights) in enumerate(level['pixels']):
        if owner==1: assert weights==[0,1];colour=(207,183,123)
        else:
            assert weights[1]==0
            value=max(0,min(1,.5+(variation-.5)*4))
            colour=(int(40+140*value),int(65+170*value),int(45+110*value))
        tile.putpixel((n%128,n//128),colour)
    x=28+i*495
    im.paste(tile.resize((448,336),Image.Resampling.NEAREST),(x,150))
    d.text((x,112),f"Footprint {level['footprint']:g} m",font=font,fill='#e7eddf')
    strip=tile.crop((0,42,128,54)).resize((448,100),Image.Resampling.NEAREST)
    im.paste(strip,(x,548))
    d.text((x,503),'Road boundary detail',font=small,fill='#c6d3cd')
    d.text((x,666),'No road weight outside its triangle owner',font=small,fill='#c6d3cd')
for i,row in enumerate(gpu['rows']):
    x=28+i*245;d.rounded_rectangle((x,730,x+225,795),radius=6,fill='#2d4a51')
    d.text((x+10,740),f'Frame {row[0]} / revision {row[1]}',font=small,fill='#e7eddf')
    d.text((x+10,769),f'Cover footprint {row[5]} m',font=small,fill='#c6d3cd')
d.text((28,841),'Viewer uses vertex colours and a shared footprint. Per-pixel filtering, anisotropy and full-world acceptance remain open.',font=small,fill='#c6d3cd')
im.save(root/'17-runtime-cover-mips.png')
print('PASS three cover mip diagnostics, hard-owner invariants and actual viewer footprint sequence')
