#!/usr/bin/env python3
"""Render Step 12 diagnostics from measured generator exports; no synthetic geometry."""
import json
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
root=Path(sys.argv[1])
assert 'failures=0' in (root/'tests.log').read_text(), 'Run passing exported tests first'
font=ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc',20)
small=ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc',14)
colors=['#b5b26a','#c6a873','#9baa65','#c9bf82','#95aa70','#a9a267']
names=['Plain / two rural exits','Unbridged river / one exit','River / independent exits','Cliff-facing town','Insufficient usable width','Plateau / engineered ramp access']
for mode,name in [('travel','12-rural-travel-cost.png'),('cores','12-patch-cores-growth.png'),('patches','12-farmland-patches.png')]:
    image=Image.new('RGB',(1500,1030),'#141e25');d=ImageDraw.Draw(image)
    d.text((24,16),'STEP 12  /  '+mode.upper()+'  /  agricultural patches, before field subdivision',font=font,fill='#e6e9df')
    for scene in range(6):
        report=json.loads((root/f'scene-{scene}.json').read_text());ox=22+(scene%3)*495;oz=72+(scene//3)*455;scale=1.12
        point=lambda p:(ox+p[0]*scale,oz+p[2]*scale)
        d.rectangle((ox,oz,ox+448,oz+358),fill='#607253')
        if mode in ['travel','cores']:
            for i,eligible in enumerate(report['eligible']):
                x=ox+i%report['columns']*8*scale;z=oz+i//report['columns']*8*scale
                t=report['travel'][i]
                if mode=='travel':
                    ratio=min(t/500,1) if t>=0 else 0
                    color=(int(75+120*ratio),int(135-60*ratio),int(160-75*ratio)) if t>=0 else '#34434b'
                else:color='#5f7251' if eligible else '#34434b'
                d.rectangle((x,z,x+8*scale,z+8*scale),fill=color)
        for c in report['reservations']:
            d.polygon([point(p) for p in c['boundary']],fill={9:'#397ea0',11:'#bdaca0',3:'#7f7975'}[c['material']])
        if 'plateau' in report:
            d.line([point(p) for p in report['plateau']+report['plateau'][:1]],fill='#9dabb0',width=3)
        if mode!='travel':
            for j,p in enumerate(report['patches']):
                if mode=='cores':
                    for i in p['cells']:
                        x=ox+i%report['columns']*8*scale;z=oz+i//report['columns']*8*scale
                        d.rectangle((x,z,x+8*scale,z+8*scale),fill=colors[j%6],outline='#768057')
                else:d.polygon([point(v) for v in p['boundary']],fill=colors[j%6],outline='#e0d5a0')
                d.line([point(v) for v in p['access']],fill='#e2cc9b',width=3)
        d.text((ox,oz+368),names[scene],font=font,fill='#e6e9df')
        d.text((ox,oz+395),f"{len(report['patches'])} patches | {sum(p['area'] for p in report['patches'])/10000:.2f} ha allocated",font=small,fill='#b7c3bb')
    d.text((24,997),'Blue: water   Grey: cliff/reservation   Beige: town   Gold: agricultural domain / track   Green: uncultivated land',font=small,fill='#b7c3bb')
    image.save(root/name)
print('PASS three diagnostic sheets from six generated fixtures')
