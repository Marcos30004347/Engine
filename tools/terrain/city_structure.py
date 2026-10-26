"""Controlled city/village road, block and building-demand planning fixtures.

Uses screened v34 domains. Arrival ports and skeleton recipes are authored fixture
requests, not solved regional transport. Nothing here allocates built households.
"""
import argparse
import json
from pathlib import Path

import shapely as sh
from shapely.geometry import LineString, Point, box, mapping, shape
from shapely.ops import nearest_points

from city_programs import BUILDING_TYPES, DISTRICT_TYPES, demand
from city_site_selection import digest
from world_atlas import fingerprint

RECIPE = 'city-structure-v1'


def polygon_parts(geometry):
    if geometry.is_empty:
        return []
    return [geometry] if geometry.geom_type=='Polygon' else [p for p in geometry.geoms if p.geom_type=='Polygon']


def node_network(roads):
    """Canonical nodes are actual intersections, not nearby unconnected points."""
    union = sh.union_all([shape(r['centreline']) for r in roads], grid_size=1e-6)
    lines = list(union.geoms) if union.geom_type=='MultiLineString' else [union]
    coords = sorted({tuple(p) for line in lines for p in (line.coords[0],line.coords[-1])})
    lookup = {p:i for i,p in enumerate(coords)}
    adjacency = {i:set() for i in range(len(coords))}
    segments=[]
    for line in sorted(lines,key=lambda l: tuple(l.coords)):
        a,b=lookup[tuple(line.coords[0])],lookup[tuple(line.coords[-1])]
        adjacency[a].add(b);adjacency[b].add(a)
        segments.append(dict(id=f'edge/{len(segments)}', a=f'node/{a}',b=f'node/{b}',
                             geometry=mapping(line),lengthMetres=line.length))
    components=[]
    unseen=set(adjacency)
    while unseen:
        component=set(); queue=[min(unseen)]
        while queue:
            n=queue.pop()
            if n in component:continue
            component.add(n);queue.extend(adjacency[n]-component)
        unseen-=component;components.append(sorted(component))
    return dict(nodes=[dict(id=f'node/{i}',point=list(p),degree=len(adjacency[i])) for i,p in enumerate(coords)],
                edges=segments,components=components)


def plan_site(site):
    domain=shape(site['localDomain'])
    if domain.geom_type!='Polygon' or not domain.is_valid or domain.interiors:
        raise ValueError('Fixture requires a valid hole-free rectangular study domain')
    x0,y0,x1,y1=domain.bounds
    if domain.symmetric_difference(box(x0,y0,x1,y1)).area > 1e-6:
        raise ValueError('Fixture study domain must be rectangular')
    width,height=x1-x0,y1-y0
    if min(width,height)<200:
        raise ValueError('Study domain too small for the fixture skeleton')
    morphology=site['morphology']
    requested=demand(morphology,site['capacity']['requestedHouseholds'])
    is_village=morphology=='lane-and-homesteads'
    cx,cy=(x0+x1)/2,(y0+y1)/2
    def pos(u,v):return (cx+u*width,cy+v*height)
    roads=[]
    def road(name,points,full_width,kind):
        line=LineString(points)
        roads.append(dict(id=name,kind=kind,widthMetres=full_width,
            centreline=mapping(line),corridor=mapping(line.buffer(full_width/2,join_style=2).intersection(domain)),
            provenance='authored-fixture-skeleton',regionalAccessStatus='unresolved'))
    road('through-lane' if is_village else 'market-street',
         [pos(-.5,-.06),pos(-.22,-.015),pos(0,0),pos(.25,.04),pos(.5,.07)],
         4 if is_village else 6,'lane' if is_village else 'main')
    if not is_village:
        road('cross-street',[pos(-.08,-.5),pos(-.015,-.24),pos(0,0),pos(.04,.23),pos(.06,.5)],5,'main')
        ring=[pos(-.27,-.19),pos(-.02,-.27),pos(.26,-.18),pos(.28,.19),pos(.02,.28),pos(-.28,.19)]
        road('district-loop',ring+[ring[0]],4,'local')
    centreline=sh.union_all([shape(r['centreline']) for r in roads])
    road_area=sh.union_all([shape(r['corridor']) for r in roads])
    public=[]
    square=box(cx-22,cy-18,cx+22,cy+18) if not is_village else box(cx-30,cy+8,cx+30,cy+28)
    public.append(dict(id='market' if not is_village else 'common',use='public-open',geometry=mapping(square)))
    if not is_village:
        px,py=pos(-.18,.17)
        civic=box(px-25,py-22,px+25,py+22)
        use='castle' if morphology=='terrace-and-bailey' else 'civic'
        public.append(dict(id=use,use=use,geometry=mapping(civic)))
    # Explicit public access connectors join the existing network at exact coordinates.
    for reserve in public:
        target=shape(reserve['geometry'])
        a,b=nearest_points(centreline,target.centroid)
        if a.distance(b)>1e-6:
            road(reserve['id']+'-access',[a.coords[0],b.coords[0]],3,'public-access')
    road_area=sh.union_all([shape(r['corridor']) for r in roads])
    public_shape=sh.union_all([shape(r['geometry']) for r in public]).intersection(domain).difference(road_area)
    for reserve in public:
        reserve['geometry']=mapping(shape(reserve['geometry']).intersection(domain).difference(road_area))
    residual=domain.difference(sh.union_all([road_area,public_shape]))
    # Village holdings begin as frontage strips along the lane, not urban blocks.
    if is_village:
        lane=shape(roads[0]['centreline'])
        development=lane.buffer(55,join_style=2).intersection(domain)
        block_shapes=polygon_parts(residual.intersection(development))
        reserved=residual.difference(development)
    else:
        block_shapes=polygon_parts(residual)
        reserved=sh.GeometryCollection()
    blocks=[]
    for polygon in sorted(block_shapes,key=lambda p:(p.centroid.x,p.centroid.y,p.area)):
        frontage=polygon.boundary.intersection(road_area.boundary).length
        # Regions meeting the study edge retain expansion reserve status.
        outer=not is_village and polygon.boundary.intersection(domain.boundary).length>1
        blocks.append(dict(id=f"{'holding-band' if is_village else 'block'}/{len(blocks)}",
            geometry=mapping(polygon),areaMetres2=polygon.area,streetFrontageMetres=frontage,
            use='growth-reserve' if outer or frontage<5 else 'homesteads' if is_village else 'residential',
            allocationBudgetMetres2=0.,buildingDemand=[],shellStatus='not-generated'))
    active=[b for b in blocks if b['use'] not in ('growth-reserve','homesteads')]
    if active:
        merchant=min(active,key=lambda b:shape(b['geometry']).centroid.distance(Point(cx+width*.12,cy)))
        merchant['use']='merchant'
        others=[b for b in active if b is not merchant]
        if others:
            crafts=min(others,key=lambda b:shape(b['geometry']).centroid.y)
            crafts['use']='craftsmen'
    allocations=[]
    for b in blocks:
        if b['use']=='growth-reserve':continue
        # Budget reserves 45% for courts, circulation, setbacks and refinement.
        b['allocationBudgetMetres2']=b['areaMetres2']*.55
        allocations.append(b)
    for reserve in public:
        if reserve['use'] in ('civic','castle'):
            allocations.append(dict(id='reservation/'+reserve['id'],use=reserve['use'],
                allocationBudgetMetres2=shape(reserve['geometry']).area*.65,buildingDemand=[]))
    remaining_budget={a['id']:a['allocationBudgetMetres2'] for a in allocations}
    ledger=[]
    # Allocate scarce specialist uses before housing that can use several districts.
    ordered=sorted(requested.items(), key=lambda item:(sum(item[0] in DISTRICT_TYPES[a['use']] for a in allocations),item[0]))
    for kind,count in ordered:
        eligible=[a for a in allocations if kind in DISTRICT_TYPES[a['use']]]
        left=count
        for allocation in sorted(eligible,key=lambda a:(-remaining_budget[a['id']],a['id'])):
            unit=BUILDING_TYPES[kind]['lotAreaMetres2']
            fitted=min(left,int(remaining_budget[allocation['id']]//unit))
            if fitted:
                allocation['buildingDemand'].append(dict(type=kind,count=fitted,
                    budgetMetres2=fitted*unit,status='budgeted-demand-not-parcels'))
                remaining_budget[allocation['id']]-=fitted*unit
                left-=fitted
        ledger.append(dict(type=kind,requested=count,budgeted=count-left,unmet=left,
                           built=0,reason='no-compatible-area-budget' if left else None))
    districts=[]
    for use in sorted({b['use'] for b in blocks}):
        members=[b for b in blocks if b['use']==use]
        district_id=site['id']+'/district/'+use
        for b in members:b['districtId']=district_id
        districts.append(dict(id=district_id,use=use,regionIds=[b['id'] for b in members],
            geometry=mapping(sh.union_all([shape(b['geometry']) for b in members])),
            allowedBuildingTypes=DISTRICT_TYPES[use],
            era='reserved-expansion' if use=='growth-reserve' else 'founding-study'))
    network=node_network(roads)
    result=dict(schema=1,recipeVersion=RECIPE,id=site['id'],regionId=site['regionId'],
        politicalOwner=site['politicalOwner'],provenance='controlled-fixture',
        status='planning-study',networkPrecisionMetres=1e-6,morphology=morphology,studyDomain=mapping(domain),
        occupiedSettlementBoundary=None,districts=districts,roads=roads,roadGraph=network,publicReservations=public,
        blocks=[] if is_village else blocks,holdingBands=blocks if is_village else [],
        ruralReserve=mapping(reserved),specialAllocations=[a for a in allocations if a['id'].startswith('reservation/')],
        buildingTypeLedger=ledger,
        householdLedger=dict(requested=site['capacity']['requestedHouseholds'],
            budgeted=sum(l['budgeted']*BUILDING_TYPES[l['type']]['householdUnits'] for l in ledger),
            allocatedToBuildings=0),
        areaLedger=dict(domain=domain.area,streets=road_area.area,publicReservations=public_shape.area,
            blocksOrHoldings=sum(b['areaMetres2'] for b in blocks),ruralReserve=reserved.area),
        unresolved=['regional-arrivals-and-supply','terrain-refined-road-grades','frontage-parcels',
                    'building-shells','growth-derived-occupied-outline'],
        buildingApproach='Dwellings-inspired direct geometry and room graphs; interiors deferred')
    result['validation']=validate_plan(result)
    if not result['validation']['passed']:
        raise ValueError(f"Invalid planning fixture: {result['validation']}")
    return result


def validate_plan(plan):
    domain=shape(plan['studyDomain'])
    regions=plan['blocks']+plan['holdingBands']
    road_area=sh.union_all([shape(r['corridor']) for r in plan['roads']])
    geometries=[road_area]+[shape(p['geometry']) for p in plan['publicReservations']]+[
        shape(b['geometry']) for b in regions]+[shape(plan['ruralReserve'])]
    union=sh.union_all(geometries)
    overlap=max(0.,sum(g.area for g in geometries)-union.area)
    mismatch=domain.symmetric_difference(union).area
    invalid=sum(not g.is_valid for g in geometries)
    graph=node_network(plan['roads'])
    disconnected=len(graph['components'])!=1
    inaccessible=sum(shape(b['geometry']).boundary.intersection(road_area.boundary).length<5
                     for b in regions if b['use']!='growth-reserve')
    allocations=[b for b in regions if b['use']!='growth-reserve']+plan['specialAllocations']
    budget_overrun=sum(sum(d['budgetMetres2'] for d in b['buildingDemand']) > b['allocationBudgetMetres2']+1e-6 for b in allocations)
    incompatible=sum(d['type'] not in DISTRICT_TYPES[b['use']] for b in allocations for d in b['buildingDemand'])
    count_mismatch=0
    for l in plan['buildingTypeLedger']:
        total=sum(d['count'] for b in allocations for d in b['buildingDemand'] if d['type']==l['type'])
        count_mismatch+=int(total!=l['budgeted'] or l['requested']!=l['budgeted']+l['unmet'])
    return dict(passed=overlap<1e-5 and mismatch<1e-5 and not(invalid or disconnected or inaccessible or budget_overrun or count_mismatch or incompatible),
        overlapMetres2=overlap,areaMismatchMetres2=mismatch,invalidGeometry=invalid,
        roadComponents=len(graph['components']),inaccessibleActiveRegions=inaccessible,
        budgetOverruns=budget_overrun,demandCountMismatches=count_mismatch,incompatibleBuildingTypes=incompatible,
        regionCount=len(regions),roadLengthMetres=sum(e['lengthMetres'] for e in graph['edges']))


def render(plans, output):
    from PIL import Image, ImageDraw, ImageFont
    image=Image.new('RGB',(1100*len(plans),1150),'#f4f1e9');draw=ImageDraw.Draw(image)
    font=ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf',22)
    small=ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf',17)
    colors={'growth-reserve':'#dbe2ce','residential':'#cfb995','merchant':'#c98f75',
            'craftsmen':'#b7a0b8','homesteads':'#aabe92','civic':'#c9af68','castle':'#b79578'}
    for index,plan in enumerate(plans):
        left=1100*index;x0,y0,x1,y1=shape(plan['studyDomain']).bounds
        scale=920/max(x1-x0,y1-y0)
        def xy(p):return left+85+(p[0]-x0)*scale,1030-(p[1]-y0)*scale
        def poly(geometry,fill,outline='#707568'):
            for p in polygon_parts(geometry):
                mask=Image.new('L',image.size,0);md=ImageDraw.Draw(mask)
                md.polygon([xy(q) for q in p.exterior.coords],fill=255)
                for h in p.interiors:md.polygon([xy(q) for q in h.coords],fill=0)
                image.paste(fill,(0,0,image.width,image.height),mask)
                if outline:
                    draw.line([xy(q) for q in p.exterior.coords],fill=outline,width=1)
                    for h in p.interiors:draw.line([xy(q) for q in h.coords],fill=outline,width=1)
        draw.text((left+35,20),f"{plan['id']} | region {plan['regionId']} | {x1-x0:g} m study width",font=font,fill='#263c35')
        draw.text((left+35,54),'Fixture streets, land allocations and building demand | no footprints yet',font=small,fill='#455b50')
        poly(shape(plan['studyDomain']),'#dbe2ce')
        for b in plan['blocks']+plan['holdingBands']:
            poly(shape(b['geometry']),colors[b['use']])
        for r in plan['roads']:poly(shape(r['corridor']),'#eee4ca',None)
        for p in plan['publicReservations']:
            poly(shape(p['geometry']),colors.get(p['use'],'#e2ce95'))
            if not shape(p['geometry']).is_empty:
                draw.text(xy(shape(p['geometry']).representative_point().coords[0]),p['id'],font=small,fill='#493f32',anchor='mm')
        for b in plan['blocks']+plan['holdingBands']:
            if b['use']=='growth-reserve':continue
            at=xy(shape(b['geometry']).representative_point().coords[0])
            count=sum(d['count'] for d in b['buildingDemand'])
            draw.text(at,f"{b['use']}\n{count} building requests",font=small,fill='#393d34',anchor='mm',align='center')
        v=plan['validation'];h=plan['householdLedger']
        draw.text((left+35,1055),f"{v['regionCount']} regions | connected roads | households budgeted {h['budgeted']}/{h['requested']}; built 0",font=small,fill='#263c35')
        draw.text((left+35,1084),'Green outer land: growth / rural reserve. Street ends: unresolved regional arrivals.',font=small,fill='#263c35')
    image.save(output)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('sites',type=Path);p.add_argument('source',type=Path);p.add_argument('output',type=Path)
    args=p.parse_args()
    if args.output.resolve()==args.source.resolve() or args.source.resolve() in args.output.resolve().parents:
        p.error('Output must be outside frozen source')
    sites=json.loads(args.sites.read_text())
    hashes={n:digest(args.source/n) for n in ['terrain-world.json','kingdom-regions.json','heightmap-metres.npy']}
    if sites['inputHashes']!=hashes:raise ValueError('Stale site studies')
    plans=[plan_site(s) for s in sorted(sites['sites'],key=lambda s:s['id'])]
    output=dict(schema=1,recipeVersion=RECIPE,sourceHashes=hashes,siteHash=digest(args.sites),
        buildingTypes=BUILDING_TYPES,districtTypes=DISTRICT_TYPES,plans=plans)
    args.output.mkdir(parents=True,exist_ok=True)
    render(plans,args.output/'city-structure.png')
    (args.output/'city-structure.json').write_text(json.dumps(output,indent=2,allow_nan=False)+'\n')
    validation=dict(deterministic=plans==[plan_site(s) for s in sorted(sites['sites'],key=lambda s:s['id'])],
        inputUnchanged=hashes=={n:digest(args.source/n) for n in hashes},
        planHash=fingerprint(plans),plans={s['id']:s['validation'] for s in plans},
        implementationHashes={n:digest(Path(__file__).with_name(n)) for n in ['city_structure.py','city_programs.py']})
    if not validation['deterministic'] or not validation['inputUnchanged']:
        raise ValueError('Determinism or frozen input check failed')
    (args.output/'validation.json').write_text(json.dumps(validation,indent=2)+'\n')
    print(json.dumps(validation))


if __name__=='__main__':main()
