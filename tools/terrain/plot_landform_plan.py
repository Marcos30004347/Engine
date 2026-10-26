#!/usr/bin/env python3
"""Draw the native landform-plan.json; does not synthesize or alter terrain."""
import argparse
import html
import json
from pathlib import Path


def render(source, output):
    plan = json.loads(source.read_text())
    cells = {c['id']: c for c in plan['shelves']}
    vertices = [p for c in cells.values() for p in c['boundary']]
    x0, x1 = min(p[0] for p in vertices), max(p[0] for p in vertices)
    z0, z1 = min(p[2] for p in vertices), max(p[2] for p in vertices)
    scale = min(720 / max(1, x1-x0), 630 / max(1, z1-z0))
    point = lambda p: (45+(p[0]-x0)*scale, 130+(z1-p[2])*scale)
    centre = lambda c: [sum(p[i] for p in c['boundary'])/len(c['boundary']) for i in range(3)]
    colours = ['#526d69', '#80928a', '#b6b497', '#ead6a8']
    highest = max(c['shelf'] for c in cells.values())
    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="1100" height="850" viewBox="0 0 1100 850">',
           '<rect width="1100" height="850" fill="#f5f1e8"/>',
           '<g font-family="system-ui,sans-serif" fill="#263d3c">',
           '<text x="45" y="49" font-size="29" font-weight="700">Land before buildings</text>',
           '<text x="45" y="79" font-size="16">Actual world cells · planned shelves · a connected uphill hierarchy</text>']
    for c in cells.values():
        points = ' '.join(f'{x:.2f},{y:.2f}' for x, y in map(point, c['boundary']))
        colour = colours[round(c['shelf']*3/max(1, highest))]
        out.append(f'<polygon points="{points}" fill="{colour}" stroke="#f5f1e8" stroke-width="1"><title>Cell {c["id"]}: +{c["uplift"]:.1f} m</title></polygon>')
    territory_path = source.parent/'city-territory.json'
    castle_area = 0
    if territory_path.exists():
        territory = json.loads(territory_path.read_text())
        for district in territory.get('regions', []):
            if district.get('use') != 'castle':
                continue
            castle_area += district['area']
            for ring in district['pieces']:
                points = ' '.join(f'{x:.2f},{y:.2f}' for x, y in map(point, ring))
                out.append(f'<polygon points="{points}" fill="#ad6049" fill-opacity=".15" stroke="#ad6049" stroke-width="2"/>')
    # Show one actual parent chain. It describes intent, not engineered access.
    route, seen, current = [], set(), plan['entrance']
    while current in cells and current not in seen:
        seen.add(current)
        route.append(point(centre(cells[current])))
        current = cells[current]['uphill']
    points = ' '.join(f'{x:.2f},{y:.2f}' for x, y in route)
    out.append(f'<polyline points="{points}" fill="none" stroke="#ad6049" stroke-width="3" stroke-dasharray="7 5"/>')
    if plan['summit'] in cells:
        x, y = point(centre(cells[plan['summit']]))
        out.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="8" fill="#ad6049" stroke="#fff" stroke-width="3"/>')
    out.append('<text x="810" y="145" font-size="21" font-weight="700">Planned uplift</text>')
    levels = sorted({(c['shelf'], c['uplift']) for c in cells.values()})
    for i, (shelf, height) in enumerate(levels):
        y = 175+i*40
        colour = colours[round(shelf*3/max(1, highest))]
        out.append(f'<rect x="810" y="{y}" width="26" height="26" rx="4" fill="{colour}"/>')
        out.append(f'<text x="849" y="{y+19}" font-size="17">+{height:g} metres</text>')
    notes = [f'{len(cells)} source cells', 'Summit stays in the inner city.', 'Its crown spans several cells.', '',
             'Heights add to world terrain.', 'Water corridors remain protected.', '',
             'Dashed line: graph ascent.', 'Stair and road engineering', 'must still certify this route.']
    for i, note in enumerate(notes):
        out.append(f'<text x="810" y="{385+i*26}" font-size="14">{html.escape(note)}</text>')
    if castle_area:
        out.append('<rect x="810" y="703" width="26" height="26" fill="#ad6049" fill-opacity=".15" stroke="#ad6049" stroke-width="2"/>')
        out.append(f'<text x="849" y="721" font-size="14">Castle: {castle_area:,.0f} m²</text>')
    length = 100*scale
    out.extend([f'<path d="M45 797h{length:.2f}" stroke="#263d3c" stroke-width="4"/>',
                '<text x="45" y="825" font-size="14">100 m</text>', '</g></svg>'])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text('\n'.join(out))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    render(args.input, args.output)
