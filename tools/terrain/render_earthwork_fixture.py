#!/usr/bin/env python3
"""Step 10 plots from C++ contour maps and actual exported CDT geometry."""
import argparse
import json
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
FONT='/System/Library/Fonts/Supplemental/Arial.ttf'
COLORS={0:'#98b483',1:'#d4c8a0',2:'#b4a786',3:'#ac7960',4:'#c5a373',5:'#898f6d',6:'#747b62',7:'#c9c7bc',9:'#4b94a5',10:'#64554d'}
def font(n):return ImageFont.truetype(FONT,n)
def page(title,subtitle):
    im=Image.new('RGB',(1440,1100),'#f3f1e9');d=ImageDraw.Draw(im)
    d.text((48,30),title,font=font(30),fill='#243c3e');d.text((48,78),subtitle,font=font(18),fill='#53605b');return im,d

def render(product,output):
    output.mkdir(parents=True,exist_ok=True);scenes={s['name']:s for s in product['scenes']}
    def map_plot(key,title,subtitle,name,wire=False):
        scene=scenes[key];m=scene['mesh'];im,d=page(title,subtitle)
        def xy(p):return (70+p[0]*10.6,1000-p[2]*10.6)
        for ids,material,layer in zip(m['triangles'],m['materials'],m['layers']):
            if layer:continue
            pts=[xy(m['vertices'][i]) for i in ids];d.polygon(pts,fill=COLORS.get(material,'#999999'))
            if wire:d.line(pts+[pts[0]],fill='#889184',width=1)
        for e in scene['mapEdges']:
            if e['layer'] or not(e['surfaceBreak'] or e['materialBoundary']):continue
            d.line([xy(m['vertices'][e['a']]),xy(m['vertices'][e['b']])],fill='#3b4b40',width=2)
        d.text((70,1030),'Ground   |   Road tread   |   Shoulder   |   Cut slope   |   Fill slope',font=font(21),fill='#243c3e')
        im.save(output/name)
    map_plot('curved-hillside','10 / Map-generated road cut and fill contours','Colors come from arrangement face materials. Dark lines are geometric or material interfaces.','10-cut-fill-map.png')
    map_plot('t-junction','10 / Unified intersection and preserved constraints','Actual CDT wireframe. The intersection has one ground-surface owner; roads are not stacked.','10-intersection-wireframe.png',True)
    scene=scenes['hillside-ditch'];im,d=page('10 / A road bench carved into the hillside','Cross-section at x = 60 m. Vertical and horizontal plot scales differ; native captures use 1x height.')
    section=[s for s in scene['section'] if 28<=s['z']<=52]
    def xy(z,y):return (100+(z-28)/24*1220,910-(y+1)/8*650)
    for y in range(-1,8):
        py=xy(28,y)[1];d.line((100,py,1320,py),fill='#d4d8cc');d.text((50,py-10),f'{y} m',font=font(17),fill='#53605b')
    d.line([xy(s['z'],s['ground']) for s in section],fill='#89658d',width=5)
    d.line([xy(s['z'],s['surface']) for s in section],fill='#a26232',width=5)
    for z in [28,32,36,40,44,48,52]:d.text((xy(z,-1)[0]-15,940),str(z),font=font(18),fill='#53605b')
    d.text((100,1000),'Transverse world position z (m)',font=font(20),fill='#243c3e')
    d.text((100,1045),'Purple: original hillside     Brown: final mesh surface, including fill, shoulder, road, ditch and cut',font=font(19),fill='#243c3e')
    im.save(output/'10-road-cut-section.png')
    scene=scenes['hydraulic-bridge'];im,d=page('10 / Bridge clearance and preserved river channel','Longitudinal section at z = 40 m, from the generated mesh layers. Deck underside is 0.6 m below the top.')
    data=[s for s in scene['longitudinal'] if 40<=s['x']<=80]
    def xy(x,y):return (100+(x-40)/40*1220,870-(y+3.5)/5*600)
    for y in [-3,-2,-1,0,1,2]:
        py=xy(40,y)[1];d.line((100,py,1320,py),fill='#d4d8cc');d.text((48,py-9),f'{y} m',font=font(17),fill='#53605b')
    for key,color in [('ground','#7c775a'),('water','#388ca5'),('deck','#555e61')]:
        points=[]
        for s in data:
            if s[key] is None:
                if len(points)>1:d.line(points,fill=color,width=5)
                points=[]
            else:points.append(xy(s['x'],s[key]))
        if len(points)>1:d.line(points,fill=color,width=5)
    deck=[s for s in data if s['deck'] is not None]
    d.line([xy(s['x'],s['deck']-.6) for s in deck],fill='#555e61',width=3)
    for s in [deck[0],deck[-1]]:d.line([xy(s['x'],s['deck']),xy(s['x'],s['natural']-.5)],fill='#555e61',width=5)
    for x in range(40,81,5):d.text((xy(x,-3.5)[0]-15,910),str(x),font=font(18),fill='#53605b')
    d.text((100,970),'Longitudinal world position x (m)',font=font(20),fill='#243c3e')
    d.text((100,1030),'The 2 m³/s hydraulic channel remains below the deck; no road fill occupies the protected river.',font=font(21),fill='#243c3e')
    im.save(output/'10-bridge-channel-section.png')
    for name,scene in scenes.items():(output.parent/f'{name}-mesh.json').write_text(json.dumps(scene))

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('product',type=Path);parser.add_argument('output',type=Path);args=parser.parse_args()
    product=json.loads(args.product.read_text())
    if product['failures'] or not all(a['pass'] for a in product['assertions']):raise SystemExit('Failed assertions cannot publish acceptance prints')
    render(product,args.output)
