#!/usr/bin/env python3
"""Render authoritative C++ district polygons; these are planning maps, not buildings."""
import argparse
import json
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

COLORS = {'castle':'#a85f64','harbor':'#539da5','civic':'#c5a65c','merchant':'#d5a86b','military':'#797d9b','prison':'#7f647e','manor':'#8ba66f','residential':'#e2c9ab','suburb':'#aebf92','craftsmen':'#b99071'}
NAMES = ['Inland fortified settlement','Navigable river-port settlement','Manor and rural settlement']
FONT='/System/Library/Fonts/Supplemental/Arial.ttf'
def font(n):return ImageFont.truetype(FONT,n)
def render(root):
    log=(root/'tests.log').read_text()
    if 'FAIL ' in log or 'failures=0' not in log:raise ValueError('Passing test evidence required')
    images=[]
    for scenario in range(3):
      for seed in (7,19,103):
        data=json.loads((root/f'{scenario}-{seed}.json').read_text())
        im=Image.new('RGB',(1200,1080),'#f5f2eb');draw=ImageDraw.Draw(im)
        draw.text((40,24),NAMES[scenario],font=font(30),fill='#2e3938')
        draw.text((40,67),f'Seed {seed} | authoritative district polygons | planning access, not engineered roads',font=font(18),fill='#52615e')
        def xy(p):return (55+p[0]*5.6,125+(140-p[1])*5.6)
        owners={cell:d['use'] for d in data['districts'] for cell in d['cells']}
        lookup={c['id']:c for c in data['cells']}
        for c in data['cells']:
            points=[xy(p) for p in c['boundary']]
            draw.polygon(points,fill='#596562' if c['excluded'] else COLORS.get(owners.get(c['id']),'#e2e3da'))
            draw.line(points+[points[0]],fill='#c3c6bb',width=1)
            if c['navigable']:
                draw.line([points[1],points[2]],fill='#167caa',width=7)
        for route in data['gateConnections']:
            if len(route)>1:draw.line([xy(p) for p in route],fill='#ffffff',width=6)
        for d in data['districts']:
            points=[xy(p) for p in d['access']]
            if len(points)>1:
                draw.line(points,fill='#faf9f4',width=4)
                for point in points[1:-1:2]:draw.ellipse((point[0]-3,point[1]-3,point[0]+3,point[1]+3),fill='#4d6663')
            p=lookup[d['anchor']]['boundary'];center=[sum(v[k] for v in p)/len(p) for k in (0,1)];x,y=xy(center)
            draw.ellipse((x-9,y-9,x+9,y+9),fill='#283c3a',outline='white',width=2)
            label=d['use'].upper();w=draw.textlength(label,font=font(14));draw.rectangle((x-w/2-4,y+12,x+w/2+4,y+32),fill='#f5f2eb');draw.text((x-w/2,y+13),label,font=font(14),fill='#263632')
        draw.text((40,932),'Gray: unassigned land | dark: excluded land | blue: verified navigable frontage',font=font(19),fill='#455551')
        draw.text((40,964),f"{len(data['districts'])} districts | {data['assignedArea']:.0f} m² assigned | {len(data['omissions'])} explicit optional omissions | {data['cdtTriangles']} CDT triangles",font=font(19),fill='#455551')
        draw.text((40,1000),'No wall circuits, buildings or farmland generated in this phase.',font=font(19),fill='#455551')
        im.save(root/f'{scenario}-{seed}-plan.png');images.append(im)
    sheet=Image.new('RGB',(1800,1620),'white')
    for i,im in enumerate(images):sheet.paste(im.resize((600,540)),((i%3)*600,(i//3)*540))
    sheet.save(root/'city-master-plan-contact.png')
    print('PASS 9 district planning prints exported from C++ polygons')
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('directory',type=Path);render(parser.parse_args().directory)
