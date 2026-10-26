#!/usr/bin/env python3
"""Step 16 diagnostic sheets from production cover pages and native CDT renders."""
import json,sys
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
root=Path(sys.argv[1]);assert json.loads((root/'validation.json').read_text())['passed']
data=[json.loads((root/f'scene-{i}-mesh.json').read_text()) for i in range(3)]
font=ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc',21)
small=ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc',15)
def canvas(title):
    im=Image.new('RGB',(1600,1070),'#17232b');ImageDraw.Draw(im).text((25,20),title,font=font,fill='#e5e8df');return im
labels=['Grass','Rock','Dry grass / bank','Road / shoulder','Water','Crop','Headland','Building','Shoulder']
colors=[(80,124,55),(137,130,105),(185,153,85),(205,184,140),(35,122,167),(180,177,71),(103,117,70),(175,131,102),(128,120,88)]
w=data[0]['coverTexture']['width'];h=data[0]['coverTexture']['height']
im=canvas('STEP 16 / normalized world-space cover weights')
d=ImageDraw.Draw(im)
for c in range(9):
    tile=Image.new('RGB',(w,h));tile.putdata([tuple(int(v*weights[c]) for v in colors[c]) for weights in data[0]['weights']]);tile=tile.resize((450,240))
    x=25+c%3*530;y=90+c//3*300;im.paste(tile,(x,y));d.text((x,y+248),labels[c],font=font,fill='#e5e8df')
d.text((25,1030),'Weights sum to one. Natural covers blend; hard domains keep exclusive ownership. No per-patch noise coordinates.',font=small,fill='#c7d0c6');im.save(root/'16-cover-weights.png')
im=canvas('STEP 16 / hard masks, crop margins and riverbank');d=ImageDraw.Draw(im)
mask=Image.new('RGB',(w,h));mask.putdata([colors[o] for o in data[0]['owners']]);mask=mask.resize((1155,771),Image.Resampling.NEAREST);im.paste(mask,(25,100))
for c in range(9):d.rectangle((1220,125+c*67,1240,145+c*67),fill=colors[c]);d.text((1255,122+c*67),labels[c],font=small,fill='#e5e8df')
d.text((25,929),'Far-footprint tests retain shoulders, water, buildings and crop headlands. Crop rows and scatter stay inside their domains.',font=small,fill='#c7d0c6');im.save(root/'16-hard-mask-check.png')
im=canvas('STEP 16 / fixed CDT boundaries, soft cover transitions');d=ImageDraw.Draw(im)
for i,x in [(0,20),(2,815)]:
    native=Image.open(root/f'scene-{i}-top.png');native.thumbnail((765,850));im.paste(native,(x,95))
    d.text((x,905),'Shading seed 1' if i==0 else 'Shading seed 991 / identical geometry',font=font,fill='#e5e8df')
d.text((25,1018),'Native Metal captures of the same CDT vertices and triangles, sampling exported cover pages in world coordinates.',font=small,fill='#c7d0c6');im.save(root/'16-patch-boundary-final.png')
im=canvas('STEP 16 / near and far footprints / identical mesh and interfaces');d=ImageDraw.Draw(im)
for i in range(2):
    for n,view in enumerate(['top','oblique']):
        native=Image.open(root/f'scene-{i}-{view}.png');native.thumbnail((760,415));x=25+i*795;y=85+n*470;im.paste(native,(x,y));d.text((x,y+424),f"{'Near: 0.25 m' if i==0 else 'Far: 4 m'} footprint / {view}",font=font,fill='#e5e8df')
d.text((25,1030),'Rows converge to their mean at distance; cover noise loses unresolved octaves. Cameras are fixed to isolate footprint filtering.',font=small,fill='#c7d0c6');im.save(root/'16-near-far-contact-sheet.png')
print('PASS four diagnostic sheets from audited cover pages and native captures')
