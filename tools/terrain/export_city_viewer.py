#!/usr/bin/env python3
"""Pack a native, world-generated city for the standalone WebGL2 fly-through.

No mesh is synthesized here: per-corner UVs, interiors, ground weights and all
native triangles inside the city's region are retained. No npm/CDN dependency.
"""
import argparse
import collections
import json
import functools
import http.server
from pathlib import Path
import shutil
import subprocess
import numpy as np
from PIL import Image, ImageDraw
from load_city_mesh import load_city_mesh
from city_history_export import attach_history

ROOT = Path(__file__).resolve().parents[2]
VERTEX = np.dtype([('position', '<f4', 3), ('uv', '<f4', 2),
                   ('normal', 'i1', 3), ('material', 'u1'), ('weights', 'u1', 3), ('pad', 'u1')])


def export(source, destination, history_path=None, region_path=None):
    destination.mkdir(parents=True, exist_ok=True)
    print(f'Loading native city: {source}', flush=True)
    data = load_city_mesh(source, preserve_instances=True)
    if not ((data.get('sourcePipeline') or '').startswith('world') or data.get('sourcePipeline') in ('native-compound', 'native-district', 'native-block')):
        raise ValueError('Expected the world → site → city pipeline export, not a layout demo')
    buildings = data['buildings']
    history = None
    if history_path:
        snapshot = json.loads(history_path.read_text())
        if snapshot.get('socialSource') == 'annual-world-story-graph':
            graph = json.loads((history_path.parent / snapshot['graphFile']).read_text())
            ancestry = {n['id']: {'id': n['id'], 'bornWeek': n['bornYear'] * 52,
                        'name': f"Resident {n['id']}", 'parents': []}
                        for n in graph['nodes'] if n['kind'] == 'person'}
            for relation, a, b in graph['edges']:
                if relation == 'parent of' and b in ancestry: ancestry[b]['parents'].append(a)
                if relation == 'partner of' and a in ancestry: ancestry[a]['spouse'] = b
            snapshot['ancestry'] = list(ancestry.values())
            del graph
        provenance_path = source.parent / 'settlement.json'
        if provenance_path.exists():
            provenance = json.loads(provenance_path.read_text()).get('historyProvenance') or {}
            if any(provenance.get(key) != snapshot.get(key) for key in ('settlement', 'week', 'year')):
                raise ValueError('History snapshot does not match the realised settlement provenance')
        else:
            raise ValueError('History inspection requires settlement.json provenance')
        history = attach_history(buildings, data.get('historicalBindings') or [], snapshot)
    if not buildings and data.get('sourcePipeline') != 'native-district':
        raise ValueError('City contains no realized buildings')
    footprints = np.array([p for b in buildings for p in b['footprint']], dtype=float) if buildings else data['vertices']
    bounds = np.array([footprints.min(axis=0), footprints.max(axis=0)])
    origin = np.array([(bounds[0, 0]+bounds[1, 0])/2, np.median([b['base'] for b in buildings]) if buildings else (bounds[0,1]+bounds[1,1])*.5,
                       (bounds[0, 2]+bounds[1, 2])/2])
    crop = bounds[:, [0, 2]] + np.array([[-24, -24], [24, 24]])
    vertices, triangles = data['vertices'], data['triangles']
    # Select only the city-region mesh, excluding distant agricultural tracks.
    # Keep intersecting triangles whole; never cut building shells or interiors.
    selected = np.zeros(len(triangles), dtype=bool)
    centres = np.empty((len(triangles), 2), dtype=np.float32)
    for begin in range(0, len(triangles), 100000):
        p = vertices[triangles[begin:begin+100000]][:, :, [0, 2]]
        selected[begin:begin+len(p)] = np.all(p.max(axis=1)>=crop[0], axis=1)&np.all(p.min(axis=1)<=crop[1], axis=1)
        centres[begin:begin+len(p)] = p.mean(axis=1)
    ids = np.flatnonzero(selected)
    groups = data['materials']
    # Full geometry is present. Groups only permit optional roof inspection.
    category = np.zeros(len(triangles), dtype=np.uint8)
    category[np.isin(groups, [14, 42]) | (data['surfaceMaterials']==2)] = 1
    category[np.isin(groups, [13, 41])] = 2
    category[np.isin(groups, [50, 51, 52])] = 3
    chunk_span = 1024 if (data.get("generation") or {}).get("mode") == "world-overview" else 64
    cells = np.floor((centres[ids]-origin[[0, 2]])/chunk_span).astype(np.int32)
    sort = np.lexsort((category[ids], cells[:, 1], cells[:, 0]))
    ids, cells = ids[sort], cells[sort]
    keys = np.column_stack((cells, category[ids]))
    splits = np.r_[0, np.flatnonzero(np.any(keys[1:] != keys[:-1], axis=1))+1, len(ids)]
    manifest_path = ROOT / data['textureManifest']
    records = json.loads(manifest_path.read_text())['materials']
    texture_dir = destination/'textures'; texture_dir.mkdir(exist_ok=True)
    for key, record in records.items():
        if 'texture' in record:
            name = f'{key}.png'
            image = Image.open(manifest_path.parent/record['texture']).convert('RGB')
            image = image.resize((512, 512), Image.Resampling.LANCZOS)
            image = Image.fromarray(np.clip(np.asarray(image)*record.get('tint', [1, 1, 1]), 0, 255).astype('uint8'))
            image.save(texture_dir/name)
            record['texture'] = f'textures/{name}'
            record.pop('tint', None)  # Baked once into the exported bitmap.
    # Material zero is a neutral fallback for native untextured pieces.
    records['0'] = {'name': 'untextured native surface', 'color': [158, 144, 122]}
    quads=data['quadInstances']
    if len(quads):
        qp=quads[:,:12].reshape(-1,4,3)
        keep=np.all(qp[:,:,[0,2]].max(axis=1)>=crop[0],axis=1)&np.all(qp[:,:,[0,2]].min(axis=1)<=crop[1],axis=1)
        quads=quads[keep].copy()
    chunks = []
    total_vertices = 0
    with (destination/'city.bin').open('wb') as output:
        for start, end in zip(splits[:-1], splits[1:]):
            ti = ids[start:end]
            p = vertices[triangles[ti]]-origin
            normal = np.cross(p[:, 1]-p[:, 0], p[:, 2]-p[:, 0])
            normal /= np.maximum(np.linalg.norm(normal, axis=1, keepdims=True), 1e-12)
            packed = np.zeros((len(ti), 3), dtype=VERTEX)
            packed['position'] = p
            packed['uv'] = data['triangleUVs'][ti]
            packed['normal'] = np.rint(normal[:, None, :]*127).astype('int8')
            packed['material'] = data['surfaceMaterials'][ti, None]
            packed['weights'] = np.rint(np.clip(data['groundWeights'][ti], 0, 1)*255).astype('uint8')
            # Deduplicate complete corners, never just positions (UV seams and normals matter).
            unique, inverse = np.unique(packed.reshape(-1), return_inverse=True)
            raw, index = unique.tobytes(), inverse.astype('<u4').tobytes()
            offset = output.tell(); output.write(raw); output.write(index)
            box = [p.min(axis=(0, 1)).tolist(), p.max(axis=(0, 1)).tolist()]
            chunks.append({'offset': offset, 'vertexBytes': len(raw), 'indexBytes': len(index),
                           'indices': len(inverse), 'bounds': box, 'category': int(category[ti[0]])})
            total_vertices += len(unique)
        # One shared indexed quad topology, grouped by spatial cell and visibility
        # category. Material is an instance attribute, so it does not split draws.
        if len(quads):
            qp=quads[:,:12].reshape(-1,4,3)-origin
            quads[:,:12]=qp.reshape(-1,12)
            qc=np.zeros(len(quads),dtype=np.uint8)
            qc[np.isin(quads[:,20],[14,42])|(quads[:,21]==2)]=1
            qc[np.isin(quads[:,20],[13,41])]=2
            qc[np.isin(quads[:,20],[50,51,52])]=3
            cells=np.floor(qp.mean(axis=1)[:,[0,2]]/64).astype(np.int32)
            order=np.lexsort((qc,cells[:,1],cells[:,0]))
            keys=np.column_stack((cells[order],qc[order]))
            split=np.r_[0,np.flatnonzero(np.any(keys[1:]!=keys[:-1],axis=1))+1,len(order)]
            for a,b in zip(split[:-1],split[1:]):
                subset=order[a:b];raw=quads[subset].astype('<f4').tobytes()
                offset=output.tell();output.write(raw)
                box=[qp[subset].min(axis=(0,1)).tolist(),qp[subset].max(axis=(0,1)).tolist()]
                chunks.append({'offset':offset,'vertexBytes':0,'indexBytes':0,'instanceBytes':len(raw),
                               'instanceCount':len(subset),'indices':6,'bounds':box,'category':int(qc[subset[0]])})
    local = lambda p: (np.asarray(p, dtype=float)-origin).tolist()
    for building in buildings:
        building['id'] = str(building['id']); building['source'] = str(building.get('source', ''))
        for key in ('footprint', 'exteriorDoors', 'interiorViews'):
            building[key] = [local(p) for p in building.get(key, [])]
        for room in building.get('roomList', []):
            room['id'] = str(room['id'])
            room['boundary'] = [local(p) for p in room.get('boundary', [])]
        building['base'] -= float(origin[1])
    completion = source.parent/'city-complete-c9.json'
    routes = []
    if completion.exists():
        for route in json.loads(completion.read_text()).get('routes', []):
            routes.append({'width': route['width'], 'path': [local([p[0], origin[1]+.035, p[2]]) for p in route['path']]})
    landforms = [{**cell, 'boundary': [local(p) for p in cell['boundary']]} for cell in data.get('landforms') or []]
    platforms = [{**deck, 'block': str(deck['block']), 'level': deck['level']-float(origin[1]),
                  'boundary': [local(p) for p in deck['boundary']]} for deck in data.get('platforms') or []]
    generation = data.get('generation') or {}
    if history:
        generation['population'] = history['population']
        generation['historyYears'] = history['year']
    report_path = source.parent/'settlement.json'
    if report_path.exists():
        world = json.loads(report_path.read_text()).get('world', {})
        for key in ('seed', 'reliefMetres', 'extentMetres', 'cellSpacingMetres'):
            if key in world:
                generation.setdefault(key, world[key])
    manifest = {'schema': 1, 'stride': VERTEX.itemsize, 'origin': origin.tolist(),
                'bounds': (bounds-origin).tolist(), 'chunks': chunks, 'materials': records,
                'buildings': buildings, 'routes': routes, 'history': history,
                'region': json.loads(region_path.read_text()) if region_path else None, 'sourcePipeline': data['sourcePipeline'],
                'generation': generation, 'landforms': landforms, 'platforms': platforms, 'structure': data.get('structure'), 'counts': data.get('counts'),
                'sourceTriangles': len(triangles)+2*len(data['quadInstances']), 'triangles': len(ids)+2*len(quads), 'vertices': total_vertices,
                'instancedPanels':len(quads), 'instanceBytes':len(quads)*88, 'instanceSchema':1,
                'bytes': (destination/'city.bin').stat().st_size,
                'buildingKinds': dict(collections.Counter(b['kind'] for b in buildings)),
                'rejections': data.get('rejections') or []}
    if data.get('groundPaint'):
        size=4096
        x0,z0=crop[0]; x1,z1=crop[1]
        mask=Image.new('RGB',(size,size),(255,0,0));draw=ImageDraw.Draw(mask)
        def pixel_ring(ring):
            return [((p[0]-x0)/(x1-x0)*(size-1),(p[1]-z0)/(z1-z0)*(size-1)) for p in ring]
        for paint in data['groundPaint']:
            if len(paint['outer'])<3: continue
            color=[0,0,0];color[paint['channel']]=255
            if not paint['holes']:
                draw.polygon(pixel_ring(paint['outer']),fill=tuple(color));continue
            shape=Image.new('L',(size,size));sd=ImageDraw.Draw(shape)
            sd.polygon(pixel_ring(paint['outer']),fill=255)
            for hole in paint['holes']:
                if len(hole)>=3: sd.polygon(pixel_ring(hole),fill=0)
            color=[0,0,0];color[paint['channel']]=255
            mask.paste(tuple(color),(0,0,size,size),shape)
        mask.save(destination/'ground-mask.png')
        manifest['groundMask']={'texture':'ground-mask.png','bounds':[float(x0-origin[0]),float(z0-origin[2]),float(x1-x0),float(z1-z0)]}
    (destination/'city.json').write_text(json.dumps(manifest, separators=(',', ':')))
    for name in ('index.html', 'viewer.js', 'viewer.css', 'inspector.js'):
        shutil.copyfile(Path(__file__).parent/'city_viewer'/name, destination/name)
    print(json.dumps({'buildings': len(buildings), 'triangles': manifest['triangles'], 'instances':len(quads), 'chunks': len(chunks),
                      'megabytes': round(manifest['bytes']/1e6, 1), 'generation': generation}), flush=True)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, help='WorldSettlementPreview city-model-c10.json')
    parser.add_argument('--output', type=Path, default=ROOT/'build/city-viewer')
    parser.add_argument('--history', type=Path, help='Authoritative history snapshot matching the native generation')
    parser.add_argument('--region', type=Path, help='Linked settlement navigation manifest')
    parser.add_argument('--generate', action='store_true', help='Run the actual world/site/city generators first')
    parser.add_argument('--seed', type=int, default=41)
    parser.add_argument('--population', type=int, default=1600)
    parser.add_argument('--relief', type=float, default=500, help='World relief in metres; default selects a gentler city landscape')
    parser.add_argument('--landforms', type=float, default=0, help='Opt-in cell-planned structural relief in metres')
    parser.add_argument('--landform-shelves', type=int, default=4)
    parser.add_argument('--executable', type=Path, default=ROOT/'build/tests/WorldSettlementPreview')
    parser.add_argument('--serve', action='store_true', help='Serve the result on localhost after export')
    parser.add_argument('--port', type=int, default=8765)
    args = parser.parse_args()
    source = args.input
    if args.generate:
        native = args.output/'native'; native.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(args.executable.resolve()), '--seed', str(args.seed), '--population', str(args.population),
                        '--city-region', '1', '--relief', str(args.relief), '--landforms', str(args.landforms), '--landform-shelves', str(args.landform_shelves), '--lots', '1', '--ground-spacing', '5', '--output', str(native.resolve())], cwd=ROOT, check=True)
        source = native/'city-model-c10.json'
    if source is None:
        parser.error('Provide --generate or --input')
    export(source.resolve(), args.output.resolve(), args.history, args.region)
    if args.serve:
        handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(args.output.resolve()))
        print(f'Fly through the city at http://127.0.0.1:{args.port}', flush=True)
        with http.server.ThreadingHTTPServer(('127.0.0.1', args.port), handler) as server:
            try:
                server.serve_forever()
            except KeyboardInterrupt:
                pass


if __name__ == '__main__':
    main()
