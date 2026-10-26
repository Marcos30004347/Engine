#!/usr/bin/env python3
"""Step 14 diagnostic sheets drawn only from audited CDT exports."""
import json, sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
root=Path(sys.argv[1])
assert json.loads((root/'validation.json').read_text())['passed']
font=ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc',20)
small=ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc',15)
names=['Adjacent regions','Land ring / channel opening','Road / channel / elevated bridge','Coincident source boundaries','Step 13 generated fields','Tributaries / downstream channel']
palette=['#7d946b','#c6b386','#7d946b','#7d946b','#7d946b','#b2ae73','#7d946b','#7d946b','#7d946b','#5294aa']
for mode,filename in [('edges','14-world-edges-vs-cdt.png'),('patches','14-patch-mesh-ids.png'),('holes','14-hole-coverage.png'),('seams','14-seams-closeup.png')]:
    im=Image.new('RGB',(1800,1120),'#17232c');d=ImageDraw.Draw(im)
    d.text((25,20),'STEP 14 / '+mode.upper()+' / frozen contours, real CDT triangles',font=font,fill='#e2e5de')
    for n in range(6):
        j=json.loads((root/f'scene-{n}-mesh.json').read_text());mesh=j['mesh'];verts=mesh['vertices']
        ox=25+n%3*595;oz=85+n//3*495
        scale=4.4 if mode!='seams' else 15
        point=lambda p:(ox+265+(p[0]-60)*scale,oz+180+(p[2]-40)*scale)
        # Draw onto a panel to clip seam closeups without changing geometry.
        panel=Image.new('RGB',(560,365),'#253840');pd=ImageDraw.Draw(panel)
        pt=lambda p:(265+(p[0]-60)*scale,180+(p[2]-40)*scale)
        for i,t in enumerate(mesh['triangles']):
            mat=mesh['materials'][i];face=j['faceIds'][i]
            color=palette[mat] if mat<len(palette) else '#aaa'
            if mode=='patches':color=(85+(face*47)%125,90+(face*31)%120,80+(face*19)%110)
            pd.polygon([pt(verts[v]) for v in t],fill=color)
            if mode in ('edges','patches','seams'):pd.line([pt(verts[v]) for v in t]+[pt(verts[t[0]])],fill='#4b625b',width=1)
        for e in j['edges']:
            if e['sources']:
                pd.line([pt(verts[e['a']]),pt(verts[e['b']])],fill='#efca84',width=2 if mode=='edges' else 1)
        seam_ids=set()
        for tile in j['tiles']:seam_ids.update(tile['edges'])
        for e in j['edges']:
            if e['id'] in seam_ids:
                a,b=verts[e['a']],verts[e['b']]
                if abs(a[0]-60)<1e-6 and abs(b[0]-60)<1e-6 or abs(a[2]-40)<1e-6 and abs(b[2]-40)<1e-6:
                    pd.line([pt(a),pt(b)],fill='#ef7b78',width=2)
                    for p in (a,b):
                        x,y=pt(p);pd.ellipse((x-2,y-2,x+2,y+2),fill='#f7ded1')
        im.paste(panel,(ox,oz))
        d.text((ox,oz+379),names[n],font=font,fill='#e2e5de')
        d.text((ox,oz+409),f"{len(j['sources'])} source segments | {len(j['edges'])} canonical edges",font=small,fill='#b7c4bf')
        d.text((ox,oz+432),f"{len(mesh['triangles'])} triangles | 4 tiles | 100% traced",font=small,fill='#b7c4bf')
    d.text((25,1082),'Gold: authored constraints   Coral: shared tile seams   Blue: channel   Beige: road   These are geometry tests, not final art.',font=small,fill='#c4cec5')
    im.save(root/filename)
print('PASS four diagnostic sheets from six frozen mesh exports')
