"""Versioned whole-world atlas inventory and layer dependency contracts.

This read-only adapter registers frozen products; it does not generate settlements.
Pending layers have no geometry, zero invented counts, and explicit blockers.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path

import numpy as np
import shapely as sh
from shapely.geometry import shape

RECIPE = 'world-atlas-v1'
# Topological order is checked, and is also the stable display/commit order.
LAYERS = (
    ('geography', (), 'Coast and regional geometry'),
    ('terrain', ('geography',), 'Height and coarse slope'),
    ('hydrology', ('geography', 'terrain'), 'Rivers and lakes'),
    ('biomes', ('geography', 'terrain', 'hydrology'), 'Regional biomes'),
    ('political', ('geography', 'terrain', 'biomes'), 'Political ownership and provisional seats'),
    ('site-studies', ('political', 'hydrology', 'terrain'), 'Fixture site screening'),
    ('resources-suitability', ('political', 'biomes', 'hydrology'), 'Resources and suitability'),
    ('settlement-hierarchy', ('resources-suitability', 'site-studies'), 'Settlement demand and supply'),
    ('regional-movement', ('settlement-hierarchy', 'terrain', 'hydrology'), 'Shared regional routes and crossings'),
    ('rural-land-use', ('regional-movement', 'biomes'), 'Countryside and farm access'),
    ('settlement-foundations', ('settlement-hierarchy', 'regional-movement', 'rural-land-use'), 'Founding anchors and growth'),
    ('city-fabric', ('settlement-foundations',), 'City streets, districts and defences'),
    ('village-fabric', ('settlement-foundations', 'rural-land-use'), 'Village lanes and homesteads'),
    ('blocks-holdings', ('city-fabric', 'village-fabric'), 'Urban blocks and rural holdings'),
    ('parcels-fields', ('blocks-holdings',), 'Frontage parcels and fields'),
    ('buildings', ('parcels-fields',), 'Programs, shells and floor feasibility'),
    ('interiors', ('buildings',), 'Direct partition and room-graph structural plans'),
)


def canonical_bytes(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()


def fingerprint(value):
    return hashlib.sha256(canonical_bytes(value)).hexdigest()


def file_hash(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def polygon_check(geometry, label):
    result = shape(geometry)
    if result.is_empty or result.geom_type not in ('Polygon', 'MultiPolygon') or not result.is_valid:
        raise ValueError(f'{label}: expected valid nonempty polygon geometry')
    if not np.isfinite(sh.get_coordinates(result)).all():
        raise ValueError(f'{label}: nonfinite coordinates')
    return result


def edge_id(edge):
    corners = edge['corners']
    if len(corners) != 2 or corners[0] == corners[1]:
        raise ValueError('Canonical edge needs two distinct corner IDs')
    return '-'.join(str(c) for c in sorted(corners))


def validate_inputs(world, politics, height):
    extent = world['extent']
    if not isinstance(extent, (int, float)) or not np.isfinite(extent) or extent <= 0:
        raise ValueError('Invalid world extent')
    if height.ndim != 2 or min(height.shape) < 2 or height.shape[0] != height.shape[1] or not np.isfinite(height).all():
        raise ValueError('Expected finite square heightfield with at least two samples')
    if (politics['geographyStage'], politics['seed']) != (world['stage'], world['seed']):
        raise ValueError('Political product does not match world revision/seed')
    surface = world['surface']
    if surface['side'] != height.shape[0] or not np.isclose(surface['cellMetres'], extent/(height.shape[0]-1)):
        raise ValueError('Heightfield metadata does not match extent or sample spacing')
    land = polygon_check(world['land'], 'land')
    regions = world['regions']
    ids = [r['id'] for r in regions]
    if sorted(ids) != list(range(len(ids))) or len(ids) != len(set(ids)):
        raise ValueError('Source region IDs must be unique dense IDs for regionOwners')
    owners = politics['regionOwners']
    society_ids = [s['id'] for s in politics['societies']]
    if len(set(society_ids)) != len(society_ids) or len(owners) != len(ids) or any(o not in society_ids for o in owners):
        raise ValueError('Every region must have exactly one existing political owner')
    for society in politics['societies']:
        expected = [rid for rid in sorted(ids) if owners[rid] == society['id']]
        if sorted(society['regionIds']) != expected:
            raise ValueError('Society region membership disagrees with regionOwners')
        if society['capitalCandidateRegion'] not in expected:
            raise ValueError('Provisional seat must belong to its society')
    polygons = [polygon_check(r['geometry'], f"region {r['id']}") for r in regions]
    edge_ids = [edge_id(e) for e in world['edges']]
    if len(set(edge_ids)) != len(edge_ids):
        raise ValueError('Duplicate canonical edge IDs')
    for edge in world['edges']:
        refs = edge['regions']
        if len(refs) not in (1, 2) or len(set(refs)) != len(refs) or any(r not in ids for r in refs):
            raise ValueError('Invalid shared edge region references')
    for key, field in [('rivers', 'waterGeometry'), ('lakes', 'geometry')]:
        if len({r['id'] for r in world[key]}) != len(world[key]):
            raise ValueError(f'Duplicate {key} IDs')
        for record in world[key]:
            polygon_check(record[field], f"{key}/{record['id']}")
    union = sh.union_all(polygons)
    # Measured diagnostics do not repair or silently change the source geometry.
    return dict(regions=len(regions), societies=len(society_ids),
                canonicalEdges=len(edge_ids), rivers=len(world['rivers']), lakes=len(world['lakes']),
                landAreaMetres2=float(land.area),
                regionOverlapMetres2=float(max(0., sum(p.area for p in polygons)-union.area)),
                regionLandDifferenceMetres2=float(union.symmetric_difference(land).area),
                missingPoliticalOwners=0, invalidGeometryCount=0,
                sourceSampleMetres=extent/(height.shape[0]-1))


def build_atlas(world, politics, height, sources, sites=None, city_study=None):
    metrics = validate_inputs(world, politics, height)
    if sites is not None:
        if sites.get('geographyStage') != world['stage'] or sites.get('seed') != world['seed']:
            raise ValueError('Site studies do not match the world')
        expected = {s['name']: s['sha256'] for s in sources if s['kind'] == 'frozen-input'}
        if sites.get('inputHashes') != expected:
            raise ValueError('Site studies are stale: frozen input hashes differ')
    studies=[]
    if city_study is not None:
        expected={s['name']:s['sha256'] for s in sources if s['kind']=='frozen-input'}
        site_source=next((s for s in sources if s['name']=='city-sites.json'),None)
        if city_study.get('sourceHashes')!=expected or site_source is None or city_study.get('siteHash')!=site_source['sha256']:
            raise ValueError('City structure study is stale or has no matching sites')
        studies.append(dict(id='city-structure-fixtures',status='controlled-study-not-world-integration',
            revision=fingerprint(city_study),sourceRef='city-structure.json',
            regionIds=sorted(p['regionId'] for p in city_study['plans']),
            scope=['roads','blocks','holding-bands','building-type-demand'],
            acceptance='upstream-supply-and-regional-arrivals-unresolved'))
    region_list = sorted(world['regions'], key=lambda r: r['id'])
    content = {
        'geography': dict(extent=world['extent'], land=world['land'],
            regions=[dict(id=r['id'], geometry=r['geometry']) for r in region_list],
            edges=sorted(world['edges'], key=edge_id)),
        'terrain': dict(surface=world['surface'], sampleHash=hashlib.sha256(
            np.asarray(height, dtype='<f8').tobytes(order='C')).hexdigest()),
        'hydrology': dict(rivers=sorted(world['rivers'], key=lambda r: r['id']),
                          lakes=sorted(world['lakes'], key=lambda r: r['id'])),
        'biomes': [dict(id=r['id'], biome=r['biome']) for r in region_list],
        'political': dict(owners=politics['regionOwners'],
                          societies=sorted(politics['societies'], key=lambda s: s['id'])),
    }
    if sites is not None:
        content['site-studies'] = sites
    counts = dict(geography=metrics['regions'], terrain=int(height.size),
                  hydrology=metrics['rivers']+metrics['lakes'], biomes=metrics['regions'],
                  political=metrics['societies'])
    if sites is not None:
        counts['site-studies'] = len(sites['sites'])
    references = {
        'geography': ['terrain-world.json#/land', 'terrain-world.json#/regions', 'terrain-world.json#/edges'],
        'terrain': ['heightmap-metres.npy', 'terrain-world.json#/surface'],
        'hydrology': ['terrain-world.json#/rivers', 'terrain-world.json#/lakes'],
        'biomes': ['terrain-world.json#/regions/*/biome'],
        'political': ['kingdom-regions.json#/societies', 'kingdom-regions.json#/regionOwners'],
        'site-studies': ['city-sites.json#/sites'],
    }
    layers, revisions = [], {}
    prefix = f"world/{world['seed']}"
    for name, deps, title in LAYERS:
        if any(d not in revisions for d in deps):
            raise ValueError('Layer dependencies must exist earlier in topological order')
        available = name in content
        revision = fingerprint(dict(recipe=RECIPE, layer=name, content=content.get(name),
                                    parents={d: revisions[d] for d in deps}))
        revisions[name] = revision
        layers.append(dict(id=f'{prefix}/layer/{name}', key=name, title=title,
            recipeVersion=RECIPE, revision=revision,
            dependencies=[f'{prefix}/layer/{d}' for d in deps],
            dependencyRevisions={f'{prefix}/layer/{d}': revisions[d] for d in deps},
            status='generated' if available else 'planned',
            validationStatus='source-contract-checked' if available else 'pending',
            visualStatus='not-reviewed', count=counts.get(name),
            sourceRefs=references.get(name, []) if available else [],
            blockers=(['fixture-only; supply, roads and settlement acceptance pending']
                      if name == 'site-studies' and available else
                      ['provisional seats; settlement feasibility pending'] if name == 'political' else
                      [] if available else ['generator-not-integrated']),
            units='metres; areas in square metres; grade as rise/run'))
    features = []
    for r in region_list:
        features.append(dict(id=f"{prefix}/region/{r['id']}", sourceId=r['id'],
            ownerRef=f"{prefix}/polity/{politics['regionOwners'][r['id']]}",
            sourceRef=f"terrain-world.json#/regions[id={r['id']}]"))
    for kind, records in [('edge', world['edges']), ('river', world['rivers']), ('lake', world['lakes'])]:
        for record in sorted(records, key=edge_id if kind == 'edge' else lambda r: r['id']):
            source_id = edge_id(record) if kind == 'edge' else record['id']
            f = dict(id=f"{prefix}/{kind}/{source_id}", sourceId=source_id)
            if kind == 'edge':
                f['regionRefs'] = [f'{prefix}/region/{r}' for r in sorted(record['regions'])]
            features.append(f)
    for society in sorted(politics['societies'], key=lambda s: s['id']):
        features.append(dict(id=f"{prefix}/polity/{society['id']}",
            seatStatus='provisional', seatRegionRef=f"{prefix}/region/{society['capitalCandidateRegion']}"))
    return dict(schema=1, recipeVersion=RECIPE, id=f'{prefix}/atlas', seed=world['seed'],
        geographyStage=world['stage'], coordinateFrame=dict(horizontalAxes=['worldX', 'worldY'],
            verticalAxis='elevation', units='metres', bounds=[0,0,world['extent'],world['extent']],
            rasterOrder='row=worldY, column=worldX; includes both extent endpoints'),
        sources=copy.deepcopy(sorted(sources, key=lambda s: s['name'])), layers=layers, features=features,
        metrics=metrics, studyProducts=studies, atlasRevision=fingerprint(dict(layers=revisions, studies=studies)),
        identityScope='world seed + source IDs; graph topology changes can replace IDs',
        sourceReferenceSyntax='logical source names; /regions[id=N] is an ID selector, not a JSON Pointer',
        dependencyScope='layer-wide invalidation; per-object reconciliation is a later phase',
        structuralSolver='Dwellings-inspired direct geometry + room graph; interiors deferred')


def changes(previous, current):
    old = {l['id']: l for l in previous['layers']}
    new = {l['id']: l for l in current['layers']}
    return dict(added=sorted(new.keys()-old.keys()), removed=sorted(old.keys()-new.keys()),
        invalidated=sorted(k for k in new.keys() & old.keys() if new[k]['revision'] != old[k]['revision']),
        unchanged=sorted(k for k in new.keys() & old.keys() if new[k]['revision'] == old[k]['revision']))


def render_baseline(world, politics, height, output):
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf', 19)
    small = ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf', 15)
    sheet = Image.new('RGB', (1440, 1570), '#f4f1e9')
    draw = ImageDraw.Draw(sheet)
    draw.text((25, 15), 'Whole-world baseline atlas | 72 km v34 input' if world['extent']==72000
              else 'Whole-world baseline atlas', font=font, fill='#263c35')
    extent = world['extent']
    side = 650
    palette = ['#b6cba0','#dabd84','#cb9f8a','#a4c9c8','#b4a4c8','#d2b5c5','#b7c2d0']
    biome_colors = {'grassland':'#bbcc91','woodland':'#759765','rainforest':'#467e69',
        'desert':'#dfc391','dry-scrub':'#b3b386','cold-heath':'#afb9b7',
        'freshwater':'#669fb7','riparian':'#6ba39a'}
    for index, name in enumerate(['Height and canonical water', 'Coarse grade', 'Regional biomes', 'Political owners / provisional seats']):
        x0, y0 = 30+(index%2)*710, 65+(index//2)*745
        draw.text((x0,y0), name, font=font, fill='#263c35')
        top = y0+30
        def xy(point):
            return x0+point[0]/extent*side, top+side-point[1]/extent*side
        def polygons(geometry, fill, outline=None):
            for poly in getattr(geometry, 'geoms', [geometry]):
                draw.polygon([xy(p) for p in poly.exterior.coords], fill=fill, outline=outline)
                for hole in poly.interiors:
                    draw.polygon([xy(p) for p in hole.coords], fill='#f4f1e9')
        if index < 2:
            if index == 0:
                data = np.clip(height/max(float(height.max()),1)*220+25, 0, 255).astype('uint8')
            else:
                gy,gx = np.gradient(height, extent/(height.shape[0]-1))
                data = (255-np.clip(np.hypot(gx,gy)/.5,0,1)*230).astype('uint8')
            sheet.paste(Image.fromarray(np.flipud(data)).convert('RGB').resize((side,side)), (x0,top))
        else:
            for r in world['regions']:
                color = biome_colors.get(r['biome'],'#ccc') if index==2 else palette[politics['regionOwners'][r['id']]%len(palette)]
                polygons(shape(r['geometry']), color, '#707868')
        if index != 1:
            for r in world['rivers']:
                polygons(shape(r['waterGeometry']), '#408db5')
            for lake in world['lakes']:
                polygons(shape(lake['geometry']), '#408db5')
        if index==3:
            for s in politics['societies']:
                p = next(r for r in world['regions'] if r['id']==s['capitalCandidateRegion'])['site']
                x,y=xy(p)
                draw.ellipse((x-4,y-4,x+4,y+4), fill='#252e29')
                draw.text((x+6,y),str(s['id']),font=small,fill='#252e29')
        legends = [f"Gray: 0 to {height.max():.0f} m | blue: canonical water",
                   'White: level | dark: grade >= 50% | coarse source only',
                   'Tan: desert/scrub | green: grass/wood/rainforest',
                   'Colours: owners | numbered dots are unvalidated seats']
        draw.text((x0,top+side+8), legends[index],font=small,fill='#263c35')
    draw.text((30,1530), 'All panels: same extent, +Y up | settlements, roads, parcels, buildings and interiors remain pending', font=small,fill='#263c35')
    sheet.save(output)


def write_index(atlas, output):
    lines = ['# Whole-world generation atlas', '', f"Source: {atlas['geographyStage']} / seed {atlas['seed']}", '',
             '[Baseline maps](baseline-atlas.png) · [Manifest](atlas.json) · [Validation](validation.json)', '',
             '| Layer | State | Count | Remaining work |', '|---|---|---:|---|']
    for layer in atlas['layers']:
        lines.append(f"| {layer['title']} | {layer['status']} | {layer['count'] if layer['count'] is not None else '—'} | {'; '.join(layer['blockers'])} |")
    if atlas['studyProducts']:
        lines += ['', '## Controlled planning studies', '',
                  'These studies do not mark pending world-generation layers as complete.']
        for study in atlas['studyProducts']:
            source=next(s for s in atlas['sources'] if s['name']==study['sourceRef'])
            directory=Path(source['path']).parent
            lines.append(f"- [{study['id']}]({directory}/review.md) · [Map]({directory}/city-structure.png): {study['acceptance']}")
    lines += ['', 'Source contract checks are recorded separately from visual/settlement acceptance.',
              'All generated layers here are imported baseline products. No pending layer is represented as a successful empty export.',
              '', '## Frozen inputs', '']
    for source in atlas['sources']:
        lines.append(f"- [{source['name']}]({source['path']}): `{source['sha256']}`")
    (output/'atlas.md').write_text('\n'.join(lines)+'\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--sites', type=Path)
    parser.add_argument('--previous', type=Path)
    parser.add_argument('--city-study', type=Path)
    args = parser.parse_args()
    if args.output.resolve() == args.source.resolve() or args.source.resolve() in args.output.resolve().parents:
        parser.error('Output must be outside the frozen source directory')
    names = ['terrain-world.json','kingdom-regions.json','heightmap-metres.npy']
    sources = [dict(name=name, path=os.path.relpath(args.source/name,args.output),
                    sha256=file_hash(args.source/name), kind='frozen-input') for name in names]
    legacy = ['WorldCityLayout', 'WorldCityMasterPlan', 'WorldCityWalls', 'WorldCityBuildingRegions']
    repo = Path(__file__).resolve().parents[2]
    for name in legacy:
        path = repo/'src/runtime/procedural/world'/f'{name}.cpp'
        if path.is_file():
            sources.append(dict(name=path.name, path=os.path.relpath(path,args.output),
                                sha256=file_hash(path), kind='historical-prototype-not-integrated'))
    sites = None
    if args.sites:
        sites = json.loads(args.sites.read_text())
        sources.append(dict(name='city-sites.json', path=os.path.relpath(args.sites,args.output),
                            sha256=file_hash(args.sites), kind='fixture-input'))
    city_study=None
    if args.city_study:
        city_study=json.loads(args.city_study.read_text())
        sources.append(dict(name='city-structure.json',path=os.path.relpath(args.city_study,args.output),
                            sha256=file_hash(args.city_study),kind='controlled-planning-study'))
    world = json.loads((args.source/names[0]).read_text())
    politics = json.loads((args.source/names[1]).read_text())
    height = np.load(args.source/names[2], allow_pickle=False)
    atlas = build_atlas(world, politics, height, sources, sites, city_study)
    prior = json.loads(args.previous.read_text()) if args.previous else None
    args.output.mkdir(parents=True,exist_ok=True)
    render_baseline(world, politics, height, args.output/'baseline-atlas.png')
    again = build_atlas(world, politics, height, sources, sites, city_study)
    validation = dict(deterministic=atlas==again,
        inputsUnchanged=all(file_hash(args.output/s['path'])==s['sha256'] for s in sources),
        availableLayers=sum(l['status']=='generated' for l in atlas['layers']),
        pendingLayers=sum(l['status']=='planned' for l in atlas['layers']), metrics=atlas['metrics'],
        implementationHash=file_hash(Path(__file__)))
    if not validation['deterministic'] or not validation['inputsUnchanged']:
        raise ValueError('Input changed or deterministic contract failed during export')
    for name, value in [('atlas.json',atlas),('validation.json',validation)]:
        (args.output/name).write_text(json.dumps(value,indent=2,allow_nan=False)+'\n')
    if prior is not None:
        (args.output/'changes.json').write_text(json.dumps(changes(prior,atlas),indent=2)+'\n')
    write_index(atlas,args.output)
    print(json.dumps(validation))


if __name__ == '__main__':
    main()
