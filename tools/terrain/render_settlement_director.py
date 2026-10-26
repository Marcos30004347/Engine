#!/usr/bin/env python3
"""Draw actual exported dwelling geometry and accepted growth snapshots."""
import json,argparse,math
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
FONT='/System/Library/Fonts/Supplemental/Arial.ttf'
ROOM_COLORS=['#d1d8ad','#b4c8d4','#dfbb86','#d8cc9d','#c8b4a6','#c7cbc5','#d5c1b0']
ROOM_NAMES=['Living / kitchen','Sleeping','Shop / work','Store','Common','Service','Hall']
BUILDING_COLORS=['#bba27e','#c69b66','#aa9b8b','#a1b8ba','#bab8a2','#c3b17d']
def floor_panel(data,index,title,size=(800,850)):
    im=Image.new('RGB',size,'#f3efdf');d=ImageDraw.Draw(im);floor=data['floors'][index]
    points=[v for p in floor['slabs'] for v in p];x0=min(v[0] for v in points);x1=max(v[0] for v in points);z0=min(v[2] for v in points);z1=max(v[2] for v in points)
    scale=min((size[0]-90)/(x1-x0),(size[1]-160)/(z1-z0))
    def xy(v):return (45+(v[0]-x0)*scale,size[1]-90-(v[2]-z0)*scale)
    def polygon(p,fill):d.polygon([xy(v) for v in p],fill=fill)
    for slab in floor['slabs']:polygon(slab,'#e9e3d1')
    for c in floor['circulation']:polygon(c,'#faf7e8')
    for r in data['rooms']:
        if r['floor']!=index:continue
        polygon(r['boundary'],ROOM_COLORS[r['use']]);vs=r['boundary'];x=sum(v[0] for v in vs)/len(vs);z=sum(v[2] for v in vs)/len(vs);at=xy([x,0,z]);d.text(at,ROOM_NAMES[r['use']],anchor='mm',font=ImageFont.truetype(FONT,17),fill='#33463c');d.text((at[0],at[1]+22),f"{r['area']:.1f} m²",anchor='mm',font=ImageFont.truetype(FONT,15),fill='#546056')
    for w in data['walls']:
        if w['floor']==index:d.line([xy(v) for v in w['path']],fill='#53564a',width=max(2,round(w['thickness']*scale)))
    walls={w['id']:w for w in data['walls']}
    for door in data['doors']:
        if door['floor']!=index:continue
        w=walls[door['wall']];a,b=w['path'];length=math.hypot(b[0]-a[0],b[2]-a[2]);dx=(b[0]-a[0])/length;dz=(b[2]-a[2])/length;c=door['center'][0];p=[c[0]-dx*door['width']/2,0,c[2]-dz*door['width']/2];q=[c[0]+dx*door['width']/2,0,c[2]+dz*door['width']/2];d.line([xy(p),xy(q)],fill='#faf7e8',width=max(4,round(w['thickness']*scale)+2));d.line([xy(p),xy([p[0]-dz*door['width'],0,p[2]+dx*door['width']])],fill='#906c4d',width=2)
    for stair in data['stairs']:
        polygon(stair['opening'],'#c7c4b1');a,b,_,c=stair['opening'];n=stair['risers']-1
        for i in range(n+1):
            t=i/n;p=[a[k]+(c[k]-a[k])*t for k in range(3)];q=[b[k]+(c[k]-a[k])*t for k in range(3)];d.line([xy(p),xy(q)],fill='#686e5d',width=2)
        mid=[sum(v[k] for v in stair['opening'])/4 for k in range(3)];at=xy(mid);d.text((at[0],at[1]),'UP' if index==0 else 'DOWN',anchor='mm',font=ImageFont.truetype(FONT,12),fill='#394536')
    d.text((30,20),title,font=ImageFont.truetype(FONT,25),fill='#30473b');d.text((30,size[1]-58),f"Floor {index} · {floor['roomArea']:.1f} m² usable rooms",font=ImageFont.truetype(FONT,20),fill='#435944');d.text((30,size[1]-28),'Walls, doors, floor openings and stairs from C++ export',font=ImageFont.truetype(FONT,15),fill='#697362');return im

def overview(states,out):
    size=(2200,840);im=Image.new('RGB',size,'#f3efdf');d=ImageDraw.Draw(im)
    d.text((35,22),'Population, programmes and retained growth',font=ImageFont.truetype(FONT,32),fill='#30473b')
    maxX=480;maxZ=216
    for k,s in enumerate(states):
        xoff=k*730+30;scale=1.38
        def xy(v):return (xoff+v[0]*scale,130+(maxZ-v[2])*scale)
        occupied={t['building'] for t in s['tenancies']}
        for b in s['buildings']:
            for slab in b['interior']['floors'][0]['slabs']:d.polygon([xy(v) for v in slab],fill=BUILDING_COLORS[b['kind']],outline='#65715f')
            if b['id'] in occupied:
                p=b['interior']['floors'][0]['slabs'][0];x=sum(v[0] for v in p)/len(p);z=sum(v[2] for v in p)/len(p);a=xy([x,0,z]);d.ellipse((a[0]-2,a[1]-2,a[0]+2,a[1]+2),fill='#344e38')
        d.text((xoff,95),f"Year {s['year']}",font=ImageFont.truetype(FONT,26),fill='#30473b')
        y=510
        for line in [f"{s['population']} residents / {s['housed']} housed",f"{len(s['buildings'])} retained or added buildings",f"{s['employed']} of {s['workers']} workers placed",f"{s['unserved']} residents without reachable service"]:
            d.text((xoff,y),line,font=ImageFont.truetype(FONT,21),fill='#435944');y+=38
    d.text((35,700),'Homes   ·   Shop-houses   ·   Worker lodging   ·   Community temples',font=ImageFont.truetype(FONT,24),fill='#435944')
    d.text((35,750),'Prepared-site qualification: existing buildings and homes remain stable through growth; decline leaves vacancies.',font=ImageFont.truetype(FONT,23),fill='#435944')
    d.text((35,795),'This tests the director. It is not a replacement city road layout or a newly integrated world map.',font=ImageFont.truetype(FONT,21),fill='#697362')
    im.save(out/'growth-snapshots.png')
def capital_overview(state,out):
    im=Image.new('RGB',(1800,1900),'#f3efdf');d=ImageDraw.Draw(im);source=state['source']
    points=[v for o in source['objects'] for v in o['footprint']]
    x0=min(v[0] for v in points);x1=max(v[0] for v in points);z0=min(v[2] for v in points);z1=max(v[2] for v in points)
    scale=min(1640/(x1-x0),1550/(z1-z0))
    def xy(v):return (80+(v[0]-x0)*scale,1720-(v[2]-z0)*scale)
    for o in source['objects']:
        for part in o['parts']:d.polygon([xy(v) for v in part['footprint']],fill='#dfdccd',outline='#c1c2b1')
    for path,width in source['walls']:d.line([xy(v) for v in path],fill='#5e6d58',width=max(2,round(width*2*scale)))
    for t in source['towers']:d.polygon([xy(v) for v in t['footprint']],fill='#5e6d58')
    for b in state['buildings']:d.polygon([xy(v) for v in b['interior']['shell']],fill=BUILDING_COLORS[b['kind']],outline='#394e40')
    d.text((50,30),'Capital / population director on accepted shells',font=ImageFont.truetype(FONT,38),fill='#30473b')
    d.text((50,90),f"{state['population']} requested · {state['housed']} housed · {state['unhoused']} need further development",font=ImageFont.truetype(FONT,28),fill='#435944')
    d.text((50,140),'Coloured: fitted programmes. Grey: source buildings without an assigned programme.',font=ImageFont.truetype(FONT,24),fill='#435944')
    d.text((50,1790),'Existing shells and entrances retained; walking routes account for walls and towers.',font=ImageFont.truetype(FONT,26),fill='#435944')
    d.text((50,1840),'Synthetic terrain/water eligibility. Institutional sites require a separate prepared-site handoff.',font=ImageFont.truetype(FONT,24),fill='#697362')
    im.save(out/'capital-director.png')
if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('input',type=Path);ap.add_argument('output',type=Path);a=ap.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    read=lambda n:json.loads((a.input/n).read_text())
    panels=[floor_panel(read('dwelling-5.json'),0,'Compact cottage / direct rooms'),floor_panel(read('dwelling-0.json'),0,'Household cottage'),floor_panel(read('dwelling-2.json'),0,'Worker lodging / measured beds'),floor_panel(read('dwelling-1.json'),0,'Shop-house / ground floor'),floor_panel(read('dwelling-1.json'),1,'Shop-house / private upper floor'),floor_panel(read('dwelling-3.json'),0,'Community hall')]
    sheet=Image.new('RGB',(2400,1700),'#f3efdf')
    for i,p in enumerate(panels):sheet.paste(p,((i%3)*800,(i//3)*850))
    sheet.save(a.output/'dwelling-plans.png');overview([read(f'settlement-{year}.json') for year in [100,110,120]],a.output)

    if (a.input/"capital-population.json").exists():capital_overview(read("capital-population.json"),a.output)
