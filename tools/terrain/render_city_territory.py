#!/usr/bin/env python3
"""Render actual C++ city territory and estate exports; no generated geometry."""
import argparse
import json
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
from shapely.geometry import Polygon
from shapely.ops import unary_union

FONT='/System/Library/Fonts/Supplemental/Arial.ttf'
COLORS={'wealthy-residential':'#b9caae','worker-housing':'#cbb9a2','military':'#adbfc9','suburb':'#d9c9a0'}
LABELS={'wealthy-residential':'Wealthy dwelling compounds','worker-housing':'Worker housing','military':'Military grounds','suburb':'Outer urban district'}

def canvas(bounds,size=(2200,1800)):
    im=Image.new('RGB',size,'#f3efdf');d=ImageDraw.Draw(im)
    x0,z0,x1,z1=bounds;scale=min((size[0]-100)/(x1-x0),(size[1]-200)/(z1-z0))
    def xy(p):return (50+(p[0]-x0)*scale,size[1]-115-(p[2]-z0)*scale)
    def polygon(p,fill,outline=None):
        if p:d.polygon([xy(v) for v in p],fill=fill,outline=outline)
    return im,d,xy,polygon,scale

def territory(data,out):
    im,d,xy,pg,scale=canvas(data['bounds'])
    d.text((35,24),'Capital territory / farms, outer districts and estates',font=ImageFont.truetype(FONT,30),fill='#283d34')
    for r in data.get('originalRegions',[]):
        if r['cover']:pg(r['boundary'],{1:'#607d50',2:'#a7a797',3:'#7aa9ba'}[r['cover']])
    for r in data['regions']:
        for p in r['pieces']:pg(p,COLORS.get(r['use'],'#c8c8b0'))
    for p in data['patches']:
        pg(p['boundary'],'#b9c592','#8a996d')
        for n,f in enumerate(p['fields']):pg(f['boundary'],['#cdbb7c','#b6c183','#d9c691','#a6b682'][n%4],'#858660')
    for c in data['contours']:
        if c['role']==1:pg(c['loop'],'#e8d6ad')
    for h in data['holdings']:
        pg(h['yard'],'#d5c8a7','#a5997a')
        for b in h['buildings']:pg(b['footprint'],['#957354','#a28468','#967f6c'][b['use']],'#645c49')
    city=data.get('city')
    if city:
        pg(city['envelope'],'#cdd3ad')
        for p in city['suburbs']:pg(p,'#cdd3ad')
        for p in city['reservations']:pg(p,'#b9c1b4','#969d91')
        for c in city['contours']:
            if c['material']==1:pg(c['loop'],'#ede1c2')
        for b in city['blocks']:
            for p in b['courtyards']:pg(p,'#ded7bd')
        for o in city['objects']:
            for p in o['parts']:pg(p['footprint'],'#977456','#74644c')
        for p,w in city['walls']:d.line([xy(v) for v in p],fill='#5c6052',width=max(2,round(w*scale*2)))
        for tower in city.get('towers',[]):pg(tower['footprint'],'#777563','#484e42')
    else:
        for p in data['occupied']:pg(p,'#c5c9b8','#7b8975')
    for r in data['regions']:
        shape=unary_union([Polygon([(v[0],v[2]) for v in p]) for p in r['pieces']]);p=shape.representative_point();x,y=xy([p.x,0,p.y])
        label=LABELS.get(r['use'],r['use']);font=ImageFont.truetype(FONT,20)
        d.text((x,y),label,anchor='mm',fill='#304638',font=font)
        d.text((x,y+26),f"{r['area']/10000:.2f} ha allocated",anchor='mm',fill='#566b58',font=ImageFont.truetype(FONT,17))
    field_count=sum(len(p['fields']) for p in data['patches'])
    d.text((35,im.height-76),f"{len(data['patches'])} farm patches | {field_count} fields | {len(data['holdings'])} farmyards | {data['cropArea']/10000:.2f} ha crops",font=ImageFont.truetype(FONT,24),fill='#324b3a')
    d.text((35,im.height-38),'Outer coloured districts are allocations; their roads and buildings are pending. Terrain is a synthetic test fixture.',font=ImageFont.truetype(FONT,19),fill='#697562')
    im.save(out/'capital-territory.png')

def estate(data,out):
    im,d,xy,pg,scale=canvas((0,0,320,240),(1700,1400))
    d.text((35,24),'Wealthy quarter / larger blocks and private gardens',font=ImageFont.truetype(FONT,28),fill='#283d34')
    pg(data['envelope'],'#c2ceaf')
    for c in data['contours']:
        if c['material']==1:pg(c['loop'],'#ede1c2')
    for b in data['buildings']:
        pg(b['plot'],'#b1c495','#96aa7f')
        if b['courtyard']:pg(b['courtyard'],'#d5c8a7')
        for p in b['parts']:pg(p,'#947458','#6b6050')
    d.text((35,im.height-75),f"{data['beforeBlocks']} former blocks → {data['afterBlocks']} larger blocks | {len(data['reclaimed'])} local street links reclaimed",font=ImageFont.truetype(FONT,24),fill='#324b3a')
    d.text((35,im.height-38),'Main roads preserved. Each fitted house plot retains at least 55% open space; garden planting is future work.',font=ImageFont.truetype(FONT,19),fill='#697562')
    im.save(out/'wealthy-estate.png')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('input',type=Path);p.add_argument('output',type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    territory(json.loads((a.input/'city-territory.json').read_text()),a.output)
    estate(json.loads((a.input/'wealthy-estate.json').read_text()),a.output)
