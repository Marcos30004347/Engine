#!/usr/bin/env python3
"""Audit exported city site polygons against the actual road triangles."""
import json
from load_city_mesh import load_city_mesh
import sys
from pathlib import Path
from shapely.geometry import Polygon
from shapely.ops import unary_union

folder = Path(sys.argv[1])
# Through the loader: the mesh arrays live in the side-car binary now.
model = load_city_mesh(folder / 'city-model-c10.json')
surfaces = model['surfaces']
roads = [Polygon(s['outer'], s['holes']) for s in surfaces if s['kind'] == 0]
road_union = unary_union(roads)
triangles = []
for indices, material in zip(model['triangles'], model['materials']):
    if material == 50:
        triangles.append(Polygon([(model['vertices'][i][0], model['vertices'][i][2]) for i in indices]))
mesh_union = unary_union(triangles)
lots = {}
for s in surfaces:
    if s['kind'] == 2:
        lots.setdefault(s['owner'], []).append(Polygon(s['outer'], s['holes']))
lots = {k: unary_union(v) for k, v in lots.items()}
support_outside = 0.
support_road = 0.
for s in model['entranceStructures']:
    polygon = Polygon([(p[0], p[2]) for p in s['polygon']])
    if s['building'] in lots:
        support_outside += polygon.difference(lots[s['building']].buffer(.0002)).area
    support_road += polygon.intersection(road_union).area
buildings = model['buildings']
report = {
    'road_polygon_overlap_m2': sum(p.area for p in roads) - road_union.area,
    'road_triangle_overlap_m2': sum(p.area for p in triangles) - mesh_union.area,
    'road_mesh_difference_m2': road_union.symmetric_difference(mesh_union).area,
    'support_outside_lot_m2': support_outside,
    'support_road_overlap_m2': support_road,
    'raised_buildings': sum(b['entranceRise'] > 0 for b in buildings),
    'sideways_entrances': sum(b['entranceLayout'] == 2 for b in buildings),
    'inset_entrances': sum(b['entranceLayout'] == 1 for b in buildings),
    'chapels': sum(b['chapels'] for b in buildings),
    'chancels': sum(b['chancels'] for b in buildings),
    'counts': model['counts'],
}
completion = json.loads((folder / 'city-complete-c9.json').read_text())
poor = {lot['id'] for lot in completion['lots']
        if lot['allocation']['maximumFloors'] == 1 and lot['allocation']['residence'] == 0}
homes = [b for b in buildings if b['id'] in poor]
report['poor_homes'] = len(homes)
report['poor_window_maximum'] = max((b['windows'] for b in homes), default=0)
report['poor_footprint_maximum_m2'] = max((Polygon([(v[0], v[2]) for v in b['footprint']]).area for b in homes), default=0)
print(json.dumps(report, indent=2))
# Quantised world coordinates can leave sub-square-millimetre roundoff.
for key in ('road_polygon_overlap_m2', 'road_triangle_overlap_m2',
            'road_mesh_difference_m2', 'support_outside_lot_m2', 'support_road_overlap_m2'):
    assert abs(report[key]) < .001, (key, report[key])
assert report['poor_window_maximum'] <= 1
assert report['poor_footprint_maximum_m2'] <= 32.001
assert all(b['entranceRise'] == 0 for b in homes)
