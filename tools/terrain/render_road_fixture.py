#!/usr/bin/env python3
"""Render Step 09 diagnostics from exported C++ sections and CDT triangles.
No invented streets, terrain meshing, or image-generation model is involved.
"""
import argparse
import json
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

FONT = '/System/Library/Fonts/Supplemental/Arial.ttf'
def font(size):
    return ImageFont.truetype(FONT, size)
def sheet(title, subtitle, size=(1440, 1000)):
    im = Image.new('RGB', size, '#f3f1e9')
    d = ImageDraw.Draw(im)
    d.text((48, 30), title, font=font(30), fill='#243c3e')
    d.text((48, 78), subtitle, font=font(18), fill='#53605b')
    return im, d

def render(product, output):
    output.mkdir(parents=True, exist_ok=True)
    scenes = {s['name']: s for s in product['scenes']}
    route = scenes['ridge-detour']
    im, d = sheet('09 / Terrain-aware regional road', 'Actual C++ road triangles over a sampled terrain diagnostic. This is not a city street layout.')
    def xy(x,z): return (70+x*5.4, 520-z*5.4)
    for x,z,h in route['terrain']:
        p=xy(x-1,z+1);q=xy(x+1,z-1)
        if h is None: color='#b96853'
        else:
            t=max(0,min(1,(h+1)/5));color=(int(155+45*t),int(180+13*t),int(130+22*t))
        d.rectangle((*p,*q),fill=color)
    for t in route['triangles']:
        d.polygon([xy(p[0],p[2]) for p in t['points']],fill='#9d9278' if t['shoulder'] else '#f9e9ad')
    d.line([xy(s['center'][0],s['center'][2]) for s in route['sections']], fill='#8f6432',width=2)
    for label,sec in [('A',route['sections'][0]),('B',route['sections'][-1])]:
        x,y=xy(sec['center'][0],sec['center'][2]);d.ellipse((x-7,y-7,x+7,y+7),fill='#253f40');d.text((x-9,y+14),label,font=font(22),fill='#243c3e')
    d.text((xy(98,9)), 'Forbidden ridge',font=font(17),fill='white')
    d.text((70,925),f"{len(route['triangles'])} constrained triangles  |  max grade {route['maximumGrade']*100:.2f}%  |  max cut/fill demand {route['maximumEarthwork']:.2f} m",font=font(21),fill='#243c3e')
    im.save(output/'09-route-cost-grade.png')

    im,d=sheet('09 / Longitudinal design profiles','Natural centerline and solved heights are in metres. Endpoint elevations remain locked.')
    for row,key in enumerate(['graded-bump','hillside-level-crossfall']):
        scene=scenes[key];secs=scene['sections'];top=180+row*365
        lo=min(min(s['ground'],s['center'][1]) for s in secs)-.5;hi=max(max(s['ground'],s['center'][1]) for s in secs)+.5
        length=secs[-1]['distance']
        def plot(distance,h):return (110+distance/length*1200,top+240-(h-lo)/(hi-lo)*240)
        for i in range(5):
            h=lo+(hi-lo)*i/4;y=plot(0,h)[1];d.line((110,y,1310,y),fill='#d0d4c9');d.text((48,y-9),f'{h:.1f}',font=font(15),fill='#53605b')
        d.text((110,top-38),key.replace('-',' ').title(),font=font(22),fill='#243c3e')
        d.line([plot(s['distance'],s['ground']) for s in secs],fill='#8a628c',width=5)
        d.line([plot(s['distance'],s['center'][1]) for s in secs],fill='#bc772c',width=3)
        d.text((110,top+254),'0 m',font=font(17),fill='#53605b');d.text((1240,top+254),f'{length:.1f} m',font=font(17),fill='#53605b')
    d.text((110,925),'Purple: natural ground     Ochre: designed road     Coincident lines indicate no centerline grading needed.',font=font(19),fill='#243c3e')
    im.save(output/'09-road-longitudinal-profile.png')

    im,d=sheet('09 / Road cross-sections on a hillside','At x = 120 m: the road climbs at 5%, while natural ground slopes sideways at 30%.')
    for col,key in enumerate(['hillside-level-crossfall','hillside-crowned']):
        left=95+col*690;top=250
        def plot(z,h):return (left+280+z*56,top+300-(h-6)*140)
        d.text((left,170),'0% crossfall' if col==0 else '2% crown, separate left/right planes',font=font(23),fill='#243c3e')
        for i in range(-2,3):
            y=plot(0,6+i)[1];d.line((left,y,left+560,y),fill='#d0d4c9');d.text((left-38,y-9),f'{6+i}',font=font(16),fill='#53605b')
        d.line([plot(z,.05*120+.30*z) for z in [-5,5]],fill='#8a628c',width=5)
        sec=min(scenes[key]['sections'],key=lambda s:abs(s['center'][0]-120))
        d.line([plot(p[2],p[1]) for p in sec['points']],fill='#bc772c',width=6)
        for p in sec['points']:
            x,y=plot(p[2],p[1]);d.ellipse((x-5,y-5,x+5,y+5),fill='#243c3e')
        d.text((left,850),'Lateral distance: -5 m ... +5 m',font=font(19),fill='#53605b')
    d.text((95,925),'Purple: existing ground     Ochre: actual road surface     Cut/fill demand shown; terrain carving is Step 10.',font=font(19),fill='#243c3e')
    im.save(output/'09-road-cross-sections.png')

    integrated=[s for name,s in scenes.items() if name.startswith('settlement-link-')]
    if integrated:
        im,d=sheet('09 / Generated settlement connections','Step 08 reservations feed actual road meshes. Markers are connection anchors; city streets and blocks are pending.')
        for x,z,h in integrated[0]['terrain']:
            t=max(0,min(1,(h+1)/4));d.rectangle((*xy(x-1,z+1),*xy(x+1,z-1)),fill=(int(156+35*t),int(181+12*t),int(137+23*t)))
        anchors=set()
        for scene in integrated:
            for t in scene['triangles']:d.polygon([xy(p[0],p[2]) for p in t['points']],fill='#9d9278' if t['shoulder'] else '#f9e9ad')
            for sec in (scene['sections'][0],scene['sections'][-1]):anchors.add(tuple(sec['center']))
        for i,p in enumerate(sorted(anchors)):
            x,y=xy(p[0],p[2]);d.ellipse((x-8,y-8,x+8,y+8),fill='#243c3e');d.text((x+12,y-8),f'Anchor {i+1}',font=font(18),fill='#243c3e')
        d.text((70,925),'Each ribbon contains tread, crown and shoulder constraints. Junction surface union follows in Step 10.',font=font(20),fill='#243c3e')
        im.save(output/'09-settlement-connections.png')
    # Native capture receives the exact exported road triangles, without reconstructing terrain.
    for key in ['ridge-detour','hillside-crowned']:
        vertices=[];triangles=[];roles=[]
        for t in scenes[key]['triangles']:
            triangles.append(list(range(len(vertices),len(vertices)+3)));vertices.extend(t['points']);roles.append(2)
        (output.parent/f'{key}-mesh.json').write_text(json.dumps({'phase':9,'mesh':{'vertices':vertices,'triangles':triangles,'roles':roles}}))

if __name__ == '__main__':
    parser=argparse.ArgumentParser();parser.add_argument('product',type=Path);parser.add_argument('output',type=Path);args=parser.parse_args()
    product=json.loads(args.product.read_text())
    if product['failures'] or not all(a['pass'] for a in product['assertions']):raise SystemExit('Cannot publish passing prints for failed road assertions')
    render(product,args.output)
