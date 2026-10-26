#!/usr/bin/env python3
"""Audit saved native milestones without changing their history or geometry."""
import argparse
import json
from pathlib import Path
import numpy as np


def audit(folder):
    h = json.loads((folder / 'history.json').read_text())
    assert h['population'] == len(h['citizens'])
    assert h['population'] == sum(x['members'] for x in h['householdRecords'])
    assert len({x['id'] for x in h['citizens']}) == h['population']
    ancestry = {x['id']: x for x in h['ancestry']}
    households = {x['id'] for x in h['householdRecords']}
    buildings = {x['id'] for x in h['buildings']}
    assert len(buildings) == len(h['buildings'])
    for p in h['citizens']:
        assert p['household'] in households
        assert ancestry[p['id']]['alive']
        assert ancestry[p['id']]['settlement'] == h['settlement']
        assert p['home'] is None or p['home'] in buildings
        assert p['workBuilding'] is None or p['workBuilding'] in buildings
    for p in ancestry.values():
        for parent in p['parents']:
            assert parent in ancestry
            assert ancestry[parent]['bornWeek'] < p['bornWeek']
    for b in h['buildings']:
        assert b['builtWeek'] <= h['week']
    activities = h.get('activities', h.get('activitiesAtNoon', []))
    if activities:
        assert {a['person'] for a in activities} == {p['id'] for p in h['citizens']}
        assert len(activities) == len(h['citizens'])
    for a in activities:
        assert a['person'] in ancestry and ancestry[a['person']]['alive']
        assert a['building'] is None or a['building'] in buildings
        assert a['room'] is None  # Room binding is explicitly not implemented yet.
    result = {'year': h['year'], 'settlement': h['settlement'], 'population': h['population'], 'households': h['households'],
              'residenceHistoryComplete': h.get('residenceHistoryComplete', True),
              'exportWarnings': h.get('exportWarnings', []),
              'completedBuildings': len(h['buildings']), 'waitingHouseholds': h['waitingHouseholds'],
              'defenceStage': h['defenceStage'], 'peopleIncludingAncestors': len(ancestry)}
    if (folder / 'city-model.json').exists():
        mesh = json.loads((folder / 'city-model.json').read_text())
        vertices = np.asarray(mesh['vertices'], dtype=float)
        triangles = np.asarray(mesh['triangles'], dtype=np.int64)
        assert np.isfinite(vertices).all() and vertices.shape[1] == 3
        assert triangles.shape[1] == 3 and triangles.min() >= 0 and triangles.max() < len(vertices)
        assert len(mesh['materials']) == len(triangles)
        assert len(mesh['triangleUVs']) == len(triangles)
        assert np.isfinite(np.asarray(mesh['triangleUVs'])).all()
        bindings = mesh.get('historicalBindings', [])
        physical = {b['id'] for b in mesh['buildings']}
        assert len(bindings) == len(buildings)
        assert {b['historicalBuilding'] for b in bindings} == buildings
        resolved = [b['generatedBuilding'] for b in bindings if b['generatedBuilding'] is not None]
        assert len(set(resolved)) == len(resolved), 'two historical buildings share one native building'
        for b in bindings:
            assert b['generatedBuilding'] is None or b['generatedBuilding'] in physical
        result.update(vertices=len(vertices), triangles=len(triangles),
                      physicalBuildings=len(physical), rejected=mesh['rejections'],
                      unresolvedBindings=sum(b['generatedBuilding'] is None for b in bindings))
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('root', type=Path)
    args = parser.parse_args()
    folders = sorted(args.root.glob('year-*/settlement-*'),
                     key=lambda p: (int(p.parent.name.split('-')[1]), int(p.name.split('-')[1])))
    report = [audit(folder) for folder in folders]
    residents_by_year = {}
    for folder in folders:
        h = json.loads((folder / 'history.json').read_text())
        ids = {p['id'] for p in h['citizens']}
        previous = residents_by_year.setdefault(h['year'], set())
        assert previous.isdisjoint(ids), 'citizen counted in multiple settlements'
        previous.update(ids)
    path = args.root / 'audit.json'
    path.write_text(json.dumps(report, indent=2))
    print(path)


if __name__ == '__main__':
    main()
