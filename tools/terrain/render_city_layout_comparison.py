#!/usr/bin/env python3
"""Compare exported C++ city geometry at identical scales; no generated geometry."""
import argparse
import json
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

NAMES = ['Crossroads', 'River town / pond', 'Walled market', 'Concave port', 'Prepared hill terrace']
FONT = '/System/Library/Fonts/Supplemental/Arial.ttf'
DISTRICT_COLORS=['#b6c3c7','#a3b9c0','#e7d6a6','#d9b984','#acb8bb','#b8afb5','#c6cfa4','#d5c8ae','#d5c8ae','#c9b29b','#d6bda5','#c7b9d6','#d7c9df','#97acb5','#b9caae','#cbb9a2']
DISTRICT_LABELS=['Castle','Harbor','Civic square','Commerce','Military','Prison','Manor','Residential','Suburbs','Crafts','Guild','Temple','Church','Citadel','Wealthy housing','Worker housing']


def panel(data, title, size=(860,850), district_mode=False):
    extent=data['envelope']+[p for ring in data.get('suburbs',[]) for p in ring]+[p for t in data.get('towers',[]) for p in t['footprint']]
    x0=min(p[0] for p in extent); x1=max(p[0] for p in extent)
    z0=min(p[2] for p in extent); z1=max(p[2] for p in extent)
    scale=min((size[0]-80)/max(140,x1-x0), (size[1]-215)/max(120,z1-z0))
    image = Image.new('RGB', size, '#f3efdf')
    draw = ImageDraw.Draw(image)
    def xy(p): return (40 + (p[0]-x0)*scale, size[1]-138-(p[2]-z0)*scale)
    def polygon(points, fill, outline=None):
        if points:
            draw.polygon([xy(p) for p in points], fill=fill, outline=outline)
    draw.text((30,22), title, fill='#283d34', font=ImageFont.truetype(FONT,24))
    polygon(data['envelope'], '#cdd3ad', '#657256')
    for suburb in data.get('suburbs',[]): polygon(suburb, '#cdd3ad')
    if district_mode:
        for d in data.get('districts',[]):
            for p in d['pieces']: polygon(p,DISTRICT_COLORS[d['use']],'#9a947f')
    for hole in data['holes']: polygon(hole, '#8ebfc6', '#628e98')
    for reserve in data['reservations']: polygon(reserve, '#e6d3a0', '#968053')
    for contour in data['contours']:
        if contour['material'] == 1: polygon(contour['loop'], '#ede1c2')
    # Lot colours by district use (index = DistrictUse): poor quarters dark red,
    # workers rust, common brown, craftsmen ochre, merchants gold, wealthy pale
    # sand, manor cream; open lots pale green, courts beige.
    LOT_TINTS=['#b6c3c7','#a3b9c0','#e7d6a6','#d9b984','#acb8bb','#b8afb5','#efe2c2','#b98a62','#a04a3c','#c9a13f','#d6bda5','#c7b9d6','#d7c9df','#97acb5','#f1e4c6','#b8643c']
    LOT_NAMES={7:'Common (80/ha)',8:'Poor quarter (150/ha)',9:'Craftsmen (55/ha)',14:'Wealthy estate (10/ha)',15:'Workers (90/ha)',6:'Manor (4/ha)',3:'Merchants (65/ha)'}
    for block in data.get('blocks',[]):
        for court in block['courtyards']: polygon(court, '#ddd7bd')
        for lot in block.get('openLots',[]): polygon(lot, '#cfd6b3', '#9a9a80')
        for lot in block.get('lots',[]): polygon(lot, LOT_TINTS[block.get('use',7)%16], '#6f5a44')
    for obj in data['objects']:
        if obj['kind'] == 3: continue
        if obj['kind'] == 2: polygon(obj['footprint'], '#91ac73', '#788e61')
        if obj.get('courtyard'): polygon(obj['courtyard'], '#d7cfad')
        parts=obj.get('parts', [{'footprint':obj['footprint'],'height':obj['height']}])
        for part in parts:
            p=part['footprint']
            tint = ['#a77b57','#9b7155','#88695b','#a48a69','#817266','#8b7c65'][obj.get('buildingType',0)%6]
            polygon(p,tint,'#66594b')
            if len(p)==4:
                pairs = [(0,3),(1,2)] if sum((p[1][k]-p[0][k])**2 for k in [0,2]) >= sum((p[3][k]-p[0][k])**2 for k in [0,2]) else [(0,1),(3,2)]
                ridge = [[(p[a][k]+p[b][k])*.5 for k in range(3)] for a,b in pairs]
                if obj.get('block'):
                    a,b=ridge
                    ridge=[[a[k]+(b[k]-a[k])*t for k in range(3)] for t in [.18,.82]]
                    for end,indices in enumerate(pairs):
                        for i in indices: draw.line([xy(p[i]),xy(ridge[end])],fill='#c6ab83',width=1)
                draw.line([xy(v) for v in ridge], fill='#c6ab83', width=2)
            else:
                center=[sum(v[k] for v in p)/len(p) for k in range(3)]
                for v in p: draw.line([xy(v),xy(center)],fill='#c6ab83',width=1)
        draw.line([xy(obj['entrance']), xy(obj['access'])],fill='#fff8e4',width=2)
    for points,width in data['walls']:
        draw.line([xy(p) for p in points], fill='#625e4e', width=max(2,round(width*scale*2)))
    for tower in data.get('towers',[]): polygon(tower['footprint'],'#8c8871','#484d40')
    def center(p): return [sum(v[k] for v in p)/len(p) for k in range(3)]
    def inside(p,q):
        return all((b[0]-a[0])*(q[2]-a[2])-(b[2]-a[2])*(q[0]-a[0])>=-1e-6 for a,b in zip(p,p[1:]+p[:1]))
    for d in data.get('districts',[]):
        if not district_mode and d['use'] in [3,7,8,9]: continue
        shapes=d['pieces'] if district_mode else [p for p in data['reservations'] if any(inside(q,center(p)) for q in d['pieces'])]
        if not shapes: continue
        if not district_mode:
            for p in shapes: polygon(p,DISTRICT_COLORS[d['use']],'#797a70')
        def area(p):return abs(sum(a[0]*b[2]-a[2]*b[0] for a,b in zip(p,p[1:]+p[:1])))
        label= max(shapes,key=area)
        x,y=xy(center(label))
        text=DISTRICT_LABELS[d['use']]
        font=ImageFont.truetype(FONT,22 if size[0]>1000 else 15)
        bbox=draw.textbbox((0,0),text,font=font)
        w=bbox[2]-bbox[0];h=bbox[3]-bbox[1]
        draw.rectangle((x-w*.5-5,y-h*.5-5,x+w*.5+5,y+h*.5+8),fill=DISTRICT_COLORS[d['use']])
        draw.text((x-w*.5,y-h*.5-3),text,font=font,fill='#34433b')
    count = sum(o['kind'] != 3 for o in data['objects'])
    block_label=f"{len(data['blocks'])} street blocks" if data.get('blocks') else f"{len(data['wards'])} wards"
    summary=f"{len(data.get('districts',[]))} allocated uses | {len(data['streets'])} road segments" if district_mode else f"{block_label} | {len(data['streets'])} segments | {count} placed areas"
    draw.text((30,size[1]-65),summary,font=ImageFont.truetype(FONT,20),fill='#344b3c')
    legend='Brown: roofs   Beige: interior open space   Green: unallocated land' if data.get('blocks') else 'Brown: houses / workshops   Green: gardens   Pale: street access'
    uses=sorted({b.get('use',7) for b in data.get('blocks',[]) if b.get('lots')})
    if uses:
        x=30; y=size[1]-105
        for u in uses:
            draw.rectangle([x,y,x+18,y+18],fill=LOT_TINTS[u%16],outline='#6f5a44')
            name=LOT_NAMES.get(u, DISTRICT_LABELS[u] if u < len(DISTRICT_LABELS) else str(u))
            draw.text((x+24,y),name,fill='#283d34',font=ImageFont.truetype(FONT,17)); x+=24+len(name)*9+30
        draw.rectangle([x,y,x+18,y+18],fill='#cfd6b3',outline='#9a9a80'); draw.text((x+24,y),'open lot',fill='#283d34',font=ImageFont.truetype(FONT,17)); x+=130
        draw.rectangle([x,y,x+18,y+18],fill='#ddd7bd',outline='#9a9a80'); draw.text((x+24,y),'court',fill='#283d34',font=ImageFont.truetype(FONT,17))
    if district_mode: legend='Color: district allocation   Yellow: reserved compound   Green: remaining housing land'
    draw.text((30,size[1]-30), legend,font=ImageFont.truetype(FONT,16),fill='#57664b')
    return image


def planned_sheet(directory, output):
    """Render only exported C++ geometry; no roads or parcels are generated here."""
    titles=['Aligned blocks','District alignments / compound reservation','Long blocks / rotated alignment']
    streets_sheet=Image.new('RGB',(2580,850),'#f3efdf')
    houses_sheet=Image.new('RGB',(2580,850),'#f3efdf')
    for variant,title in enumerate(titles):
        data=json.loads((directory/f'planned-city-{variant}.json').read_text())
        im=panel(data,title)
        houses_sheet.paste(im,(variant*860,0))
        panel(data,title,(1600,1460)).save(output/f'planned-city-{variant}.png')
        street_data={**data,'objects':[],'blocks':[]}
        road=panel(street_data,title)
        draw=ImageDraw.Draw(road)
        x0=min(p[0] for p in data['envelope']);x1=max(p[0] for p in data['envelope'])
        z0=min(p[2] for p in data['envelope']);z1=max(p[2] for p in data['envelope'])
        scale=min(780/max(140,x1-x0),670/max(120,z1-z0))
        def xy(p):return (40+(p[0]-x0)*scale,747-(p[2]-z0)*scale)
        # Centerline colors show the hierarchy already assigned by C++.
        for street in data['streets']:
            color='#755037' if street['arterial'] else '#9a7150' if street.get('hierarchy',0)==2 else '#b28d60' if street.get('hierarchy',0)==1 else '#b7ab86'
            draw.line([xy(p) for p in street['path']],fill=color,width=3 if street['arterial'] or street.get('hierarchy',0)==2 else 2)
        draw.rectangle((0,780,860,850),fill='#f3efdf')
        draw.text((30,785),f"{data['subdivisionCells']} coarse cells | {len(data['blocks'])} physical blocks | {len(data['streets'])} segments",font=ImageFont.truetype(FONT,18),fill='#344b3c')
        draw.text((30,821),'Dark: arteries / avenues   Mid: district streets   Light: local streets',font=ImageFont.truetype(FONT,15),fill='#57664b')
        streets_sheet.paste(road,(variant*860,0))
    houses_sheet.save(output/'planned-alternatives.png')
    streets_sheet.save(output/'planned-streets.png')
    comparison=Image.new('RGB',(1720,850),'#f3efdf')
    baseline=json.loads((directory/'market-city.json').read_text())
    planned=json.loads((directory/'planned-city-0.json').read_text())
    comparison.paste(panel(baseline,'Village / existing organic network'),(0,0))
    comparison.paste(panel(planned,'City / planned bisection and block lots'),(860,0))
    comparison.save(output/'village-city-comparison.png')


def building_sheet(data,output):
    names=['Cottage','Shop house','Merchant house','Inn','Workshop yard','Warehouse']
    sheet=Image.new('RGB',(1440,1000),'#f3efdf')
    for program,name in enumerate(names):
        choices=[o for o in data['objects'] if o['kind'] not in [2,3] and o['buildingType']==program]
        if not choices: continue
        o=max(choices,key=lambda o:(not o.get('compactFallback',False), len(o['parts']),sum((o['footprint'][2][k]-o['footprint'][0][k])**2 for k in [0,2]),o['height']))
        im=Image.new('RGB',(480,500),'#f3efdf');draw=ImageDraw.Draw(im)
        a,b,c,d=o['footprint']
        width=((b[0]-a[0])**2+(b[2]-a[2])**2)**.5
        depth=((d[0]-a[0])**2+(d[2]-a[2])**2)**.5
        tx,tz=(b[0]-a[0])/width,(b[2]-a[2])/width
        def xy(p):
            u=(p[0]-a[0])*tx+(p[2]-a[2])*tz
            v=-(p[0]-a[0])*tz+(p[2]-a[2])*tx
            return (240+(u-width*.5)*11,370-v*11)
        draw.text((25,20),name,font=ImageFont.truetype(FONT,27),fill='#283d34')
        draw.polygon([xy(p) for p in o['footprint']],outline='#c3b996')
        if o['courtyard']:draw.polygon([xy(p) for p in o['courtyard']],fill='#d7cfad')
        for part in o['parts']:
            p=part['footprint']
            draw.polygon([xy(v) for v in p],fill='#98795c',outline='#66594b')
            pairs = [(0,3),(1,2)] if sum((p[1][k]-p[0][k])**2 for k in [0,2]) >= sum((p[3][k]-p[0][k])**2 for k in [0,2]) else [(0,1),(3,2)]
            ridge=[[(p[i][k]+p[j][k])*.5 for k in range(3)] for i,j in pairs]
            draw.line([xy(v) for v in ridge],fill='#cfb891',width=3)
        draw.line([xy(o['access']),xy(o['entrance'])],fill='#9a956b',width=3)
        draw.text((25,427),f"{width:.1f} x {depth:.1f} m plot | {len(o['parts'])} wing(s)",font=ImageFont.truetype(FONT,19),fill='#435948')
        draw.text((25,462),'Actual city geometry | all panels at the same scale',font=ImageFont.truetype(FONT,15),fill='#66715a')
        sheet.paste(im,((program%3)*480,(program//3)*500))
    sheet.save(output/'building-programs.png')


def render(before, after, output):
    output.mkdir(parents=True, exist_ok=True)
    for root in [before,after]:
        log=(root/'tests.log').read_text()
        if 'FAIL ' in log or 'failures=0' not in log:
            raise ValueError(f'Passing geometry tests required: {root}')
    metrics = []
    for recipe in range(2):
        sheet = Image.new('RGB',(1720,4250),'#f3efdf')
        for preset,name in enumerate(NAMES):
            tag=f'{preset}-{recipe}'
            pair=Image.new('RGB',(1720,850))
            for column,root in enumerate([before,after]):
                data=json.loads((root/f'{tag}.json').read_text())
                im=panel(data, f"{name} / {'Voronoi' if recipe == 0 else 'Recursive'} / {'Before' if column == 0 else 'After'}")
                pair.paste(im,(column*860,0))
                metrics.append(dict(scene=tag,version=['before','after'][column],wards=len(data['wards']),streets=len(data['streets']),placed=sum(o['kind']!=3 for o in data['objects']),rejected=data['rejected']))
            pair.save(output/f'{tag}-comparison.png')
            sheet.paste(pair,(0,preset*850))
        sheet.save(output/f'{["voronoi","recursive"][recipe]}-comparison.png')
    if (after/'market-city.json').exists():
        city=json.loads((after/'market-city.json').read_text())
        panel(city, 'Market city / mixed frontage and open courts', (1600,1460)).save(output/'market-city.png')
        building_sheet(city,output)
    if (after/'planned-city-0.json').exists(): planned_sheet(after,output)
    if (after/'large-capital.json').exists():
        large=json.loads((after/'large-capital.json').read_text())
        panel(large,'Larger capital / outer neighbourhoods and tower-linked walls',(2000,1750)).save(output/'large-capital.png')
        panel({**large,'objects':[],'blocks':[]},'Larger capital / streets and reserved precincts',(2000,1750)).save(output/'large-capital-streets.png')
    if (after/'capital-suburbs.json').exists():
        suburbs=json.loads((after/'capital-suburbs.json').read_text())
        panel(suburbs,'Capital / wall clearance and gate suburbs',(1700,1460)).save(output/'capital-suburbs.png')
    if (after/'planned-capital.json').exists():
        capital=json.loads((after/'planned-capital.json').read_text())
        panel(capital,'Capital / smaller house plots and reserved precincts',(1600,1460)).save(output/'planned-capital.png')
        district_data={**capital,'objects':[],'blocks':[]}
        panel(district_data,'Capital / district allocation',(1600,1460),district_mode=True).save(output/'capital-districts.png')
    (output/'comparison-metrics.json').write_text(json.dumps(metrics,indent=2)+'\n')


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('before',type=Path)
    parser.add_argument('after',type=Path)
    parser.add_argument('output',type=Path)
    args=parser.parse_args()
    render(args.before,args.after,args.output)
