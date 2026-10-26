#!/usr/bin/env python3
"""Step 13 plans from exported field geometry and measured acceptance metrics."""
import json,math,sys
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
root=Path(sys.argv[1])
assert 'failures=0' in (root/'tests.log').read_text()
font=ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc',18)
small=ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc',13)
colors=['#b5b977','#b3a773','#99ad6b','#c1b588','#9fab7b','#c4bd8b']
names=['Enclosed fields','Concave patch + protected hole','Triangular remnant','Inaccessible narrow neck','Clipped corner','Long strip preset','Exhausted subdivision budget','Step 12 generated patch']
for mode,name in [('grain','13-field-grain.png'),('width','13-field-width-tips.png'),('repair','13-remnant-repair.png'),('final','13-farms-final.png')]:
    im=Image.new('RGB',(1800,950),'#152128');draw=ImageDraw.Draw(im)
    draw.text((24,20),'STEP 13  /  '+mode.upper()+'  /  final field masks and measured access',font=font,fill='#e3e7df')
    for scene in range(8):
        d=json.loads((root/f'scene-{scene}.json').read_text());ox=24+scene%4*448;oz=80+scene//4*425
        loX=min(p[0] for p in d['outer']);loZ=min(p[2] for p in d['outer']);w=max(p[0] for p in d['outer'])-loX;h=max(p[2] for p in d['outer'])-loZ
        scale=min(400/w,280/h);point=lambda p:(ox+(p[0]-loX)*scale,oz+(p[2]-loZ)*scale)
        for c in d['contours']:
            mat=c['material'];color='#c38e70' if mode=='repair' else '#64744f'
            if mat==1:color='#d6c69a'
            elif mat>=20:color=colors[(mat-21)%len(colors)]
            draw.polygon([point(p) for p in c['boundary']],fill=color)
        for hpoly in d['holes']:draw.polygon([point(p) for p in hpoly],fill='#397d96')
        for j,f in enumerate(d['fields']):
            polygon=[point(p) for p in f['boundary']]
            draw.line(polygon+polygon[:1],fill='#d7d3a1',width=1)
            if mode in ('grain','final'):
                mask=Image.new('L',im.size,0);ImageDraw.Draw(mask).polygon(polygon,fill=255)
                layer=Image.new('RGB',im.size,'#879158');ld=ImageDraw.Draw(layer);angle=d['grain'];dx=math.cos(angle);dy=math.sin(angle);nx=-dy;ny=dx
                cx=sum(p[0] for p in polygon)/len(polygon);cy=sum(p[1] for p in polygon)/len(polygon)
                rows=Image.new('L',im.size,0);rd=ImageDraw.Draw(rows)
                for k in range(-45,46):
                    x=cx+nx*k*8;y=cy+ny*k*8;rd.line((x-dx*450,y-dy*450,x+dx*450,y+dy*450),fill=60,width=1)
                import PIL.ImageChops
                im.paste(layer,(0,0),PIL.ImageChops.multiply(mask,rows))
                draw=ImageDraw.Draw(im)
            if mode=='width':
                cx=sum(p[0] for p in polygon)/len(polygon);cy=sum(p[1] for p in polygon)/len(polygon)
                text=f"{f['width']:.1f}m / {f['angle']:.0f}°";box=draw.textbbox((0,0),text,font=small);draw.text((cx-box[2]/2,cy-7),text,font=small,fill='#17252a')
            if mode in ('grain','repair'):
                draw.line([point(p) for p in f['access']],fill='#f0d495',width=2)
        draw.text((ox,oz+294),names[scene],font=font,fill='#e3e7df')
        draw.text((ox,oz+323),f"{len(d['fields'])} fields | crops {d['cropArea']:.0f} m²",font=small,fill='#b7c4bc')
        draw.text((ox,oz+344),f"margins {d['marginArea']:.0f} | tracks {d['trackArea']:.0f} m²",font=small,fill='#b7c4bc')
    draw.text((24,921),'Gold/green: crops   Beige: tracks/access   Green or terracotta: retained margins   Blue: protected hole   Rows are diagram overlays.',font=small,fill='#b7c4bc')
    im.save(root/name)
print('PASS four diagnostic sheets from eight field fixtures')
