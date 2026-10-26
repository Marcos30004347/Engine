"""Conservative, deterministic city study domains on frozen world geography.

These are fixture reservations, not populated cities or foundation surveys.
Run with the same offline Python environment as generate_map_geography.py.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
import shapely as sh
from shapely.geometry import Point, box, mapping, shape

RECIPE = 'city-site-selection-v1'
# Art-direction study sizes and coarse terrain limits, not historical standards.
BRIEFS = (
    dict(id='market-town', halfExtent=400, maxGrade=.08, minHeight=10,
         maxHeight=350, households=240, morphology='market-frontage'),
    dict(id='hill-town', halfExtent=250, maxGrade=.16, minHeight=350,
         maxHeight=1000, households=100, morphology='terrace-and-bailey'),
    dict(id='open-village', halfExtent=200, maxGrade=.08, minHeight=10,
         maxHeight=350, households=30, morphology='lane-and-homesteads'),
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def terrain_metrics(height, extent, domain):
    """Conservatively measure every raster cell touching the domain's bounds."""
    dz = extent / (height.shape[0] - 1)
    x0, y0, x1, y1 = domain.bounds
    lo = np.floor(np.array([x0, y0]) / dz).astype(int)
    hi = np.ceil(np.array([x1, y1]) / dz).astype(int)
    if min(lo) < 0 or max(hi) >= height.shape[0]:
        return None
    patch = height[lo[1]:hi[1]+1, lo[0]:hi[0]+1]
    if min(patch.shape) < 2 or not np.isfinite(patch).all():
        return None
    gy, gx = np.gradient(patch, dz)
    return dict(maxGrade=float(np.hypot(gx, gy).max()),
                minHeight=float(patch.min()), maxHeight=float(patch.max()),
                reliefMetres=float(np.ptp(patch)))


def select(world, political, height, briefs=BRIEFS):
    extent = world['extent']
    if height.ndim != 2 or height.shape[0] != height.shape[1]:
        raise ValueError('Expected square scalar heightfield')
    if political['geographyStage'] != world['stage'] or political['seed'] != world['seed']:
        raise ValueError('Political product does not match geography')
    spacing = extent / (height.shape[0] - 1)
    land = shape(world['land'])
    water = sh.union_all([shape(r['waterGeometry']) for r in world['rivers']] +
                         [shape(l['geometry']) for l in world['lakes']])
    # One source sample of uncertainty plus a 30 m bank reserve.
    clearance = spacing + 30
    safe = land.buffer(-clearance).difference(water.buffer(clearance))
    regions = sorted(world['regions'], key=lambda r: r['id'])
    selected, evaluations = [], []
    for brief in briefs:
        candidates = []
        for region in regions:
            point = shape(region['geometry']).representative_point()
            x, y = point.coords[0]
            half = brief['halfExtent']
            domain = box(x-half, y-half, x+half, y+half)
            reasons = []
            if not safe.covers(domain):
                reasons.append('coast-or-water-clearance')
            if not shape(region['geometry']).covers(domain):
                reasons.append('crosses-context-region')
            metrics = terrain_metrics(height, extent, domain)
            if metrics is None:
                reasons.append('invalid-height-coverage')
            else:
                if metrics['maxGrade'] > brief['maxGrade']:
                    reasons.append('coarse-slope')
                if metrics['minHeight'] < brief['minHeight'] or metrics['maxHeight'] > brief['maxHeight']:
                    reasons.append('role-elevation')
            if any(point.distance(Point(s['centreMetres'])) < 2500 for s in selected):
                reasons.append('settlement-spacing')
            water_distance = float(point.distance(water)) if not water.is_empty else None
            # Water proximity is an opportunity only; supply is still unresolved.
            score = metrics['maxGrade'] + (water_distance or extent) / extent * .1 if metrics else None
            evaluation = dict(briefId=brief['id'], regionId=region['id'],
                              reasons=reasons, metrics=metrics, score=score)
            evaluations.append(evaluation)
            if not reasons:
                candidates.append((score, region['id'], point, domain, metrics, water_distance))
        if not candidates:
            continue
        _, rid, point, domain, metrics, distance = min(candidates, key=lambda c: c[:2])
        selected.append(dict(id=brief['id'], recipeVersion=RECIPE, provenance='fixture',
            status='selected-study-domain', seed=world['seed'], regionId=rid,
            politicalOwner=political['regionOwners'][rid], centreMetres=list(point.coords[0]),
            localDomain=mapping(domain), areaMetres2=domain.area, terrain=metrics,
            nearestSurfaceWaterMetres=distance, morphology=brief['morphology'],
            capacity=dict(requestedHouseholds=brief['households'], allocatedHouseholds=0,
                          unmetHouseholds=brief['households']),
            pending=['water-supply', 'regional-road-ports', 'local-terrain-refinement',
                     'anchors-and-growth', 'parcels-and-shells', 'structural-interiors']))
    return dict(schema=1, recipeVersion=RECIPE, geographyStage=world['stage'],
                seed=world['seed'], units='metres', sourceSampleMetres=spacing,
                waterClearanceMetres=clearance, briefs=list(briefs), sites=selected,
                omittedBriefIds=[b['id'] for b in briefs if b['id'] not in {s['id'] for s in selected}],
                evaluations=evaluations)


def render(world, product, output):
    from PIL import Image, ImageDraw, ImageFont
    image = Image.new('RGB', (1200, 1260), '#f4f1e9')
    draw = ImageDraw.Draw(image)
    font = ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf', 18)
    title = ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf', 26)
    def xy(point):
        return (40+point[0]/world['extent']*1120, 1190-point[1]/world['extent']*1080)
    def polygons(geometry, fill, outline=None):
        for poly in getattr(geometry, 'geoms', [geometry]):
            draw.polygon([xy(p) for p in poly.exterior.coords], fill=fill, outline=outline)
            for hole in poly.interiors:
                draw.polygon([xy(p) for p in hole.coords], fill='#f4f1e9')
    for region in world['regions']:
        polygons(shape(region['geometry']), '#dce2c9', '#a8b39c')
    for river in world['rivers']:
        draw.line([xy(p) for p in river['points']], fill='#4e96b0', width=2)
    for lake in world['lakes']:
        polygons(shape(lake['geometry']), '#4e96b0')
    for site, color in zip(product['sites'], ['#ac462c', '#754997', '#30734a']):
        polygons(shape(site['localDomain']), color)
        x, y = xy(site['centreMetres'])
        draw.line([(x,y), (x+20,y-30)], fill=color, width=2)
        draw.text((x+22,y-60), f"{site['id']} / region {site['regionId']}\n{site['areaMetres2']/10000:g} ha study domain", font=font, fill=color)
    draw.text((35, 20), 'Medieval settlement study regions | frozen v34 geography', font=title, fill='#263c35')
    draw.text((35, 60), 'Coloured domains: proposed occupied land. Pale cells: political context. Interiors pending.', font=font, fill='#445d50')
    draw.line([(45,1215),(45+10000/world['extent']*1120,1215)], fill='#263c35', width=3)
    draw.text((45,1224), '10 km | world +Y upwards', font=font, fill='#263c35')
    image.save(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.source.resolve() == args.output.resolve():
        parser.error('Output must differ from the frozen input directory')
    names = ['terrain-world.json', 'kingdom-regions.json', 'heightmap-metres.npy']
    hashes = {name: digest(args.source/name) for name in names}
    world = json.loads((args.source/names[0]).read_text())
    political = json.loads((args.source/names[1]).read_text())
    height = np.load(args.source/names[2])
    result = select(world, political, height)
    result['inputHashes'] = hashes
    result['implementationHash'] = digest(Path(__file__))
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output/'city-sites.json').write_text(json.dumps(result, indent=2, allow_nan=False)+'\n')
    render(world, result, args.output/'city-sites.png')
    validation = dict(selected=len(result['sites']), omitted=result['omittedBriefIds'],
        inputUnchanged=hashes == {name: digest(args.source/name) for name in names},
        deterministic=result == dict(select(world, political, height), inputHashes=hashes,
                                      implementationHash=digest(Path(__file__))))
    (args.output/'validation.json').write_text(json.dumps(validation, indent=2)+'\n')
    print(json.dumps(validation))


if __name__ == '__main__':
    main()
