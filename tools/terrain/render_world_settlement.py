#!/usr/bin/env python3
"""Render the actual-world settlement preview with the existing C++-export renderers.

Consumes WorldSettlementPreview output (city-territory.json, village-layout.json,
overview.svg, domain.svg); no geometry is generated here.
"""
import argparse, json, subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import render_city_territory, render_city_layout_comparison, render_settlement_director
from PIL import Image

def main():
    p = argparse.ArgumentParser(); p.add_argument('directory', type=Path); a = p.parse_args()
    root = a.directory
    territory = json.loads((root / 'city-territory.json').read_text())
    render_city_territory.territory(territory, root, note=territory.get('suitabilitySource', ''), title='Settlement territory on the actual generated world')
    (root / 'capital-territory.png').rename(root / 'territory.png')
    village = root / 'village-layout.json'
    if village.exists():
        data = json.loads(village.read_text())
        title = f"Village on the actual world: {len(data['streets'])} streets, {data['housed']} housed / {data['unhoused']} unhoused"
        render_city_layout_comparison.panel(data, title, (1600, 1460)).save(root / 'village.png')
        render_city_layout_comparison.panel(data, 'District allocation', (1600, 1460), district_mode=True).save(root / 'village-districts.png')
    dwellings = sorted(root.glob('dwelling-*.json'))
    if dwellings:
        panels = []
        for path in dwellings[:6]:
            data = json.loads(path.read_text())
            for floor in range(len(data['floors'])):
                panels.append(render_settlement_director.floor_panel(data, floor, f"{data.get('programme', 'dwelling')} / floor {floor} / {data['residents']} residents"))
        panels = panels[:6]
        sheet = Image.new('RGB', (800 * min(3, len(panels)), 850 * ((len(panels) + 2) // 3)), '#f3efdf')
        for i, panel in enumerate(panels):
            sheet.paste(panel, ((i % 3) * 800, (i // 3) * 850))
        sheet.save(root / 'dwellings.png')
    svgs = [str(f) for f in ('overview.svg', 'domain.svg', 'blocks.svg', 'compounds.svg') if (root / f).exists()]
    if svgs:
        subprocess.run(['qlmanage', '-t', '-s', '1600', '-o', str(root)] + [str(root / f) for f in svgs], check=True, capture_output=True)
    print('rendered', [f.name for f in sorted(root.glob('*.png'))])

if __name__ == '__main__':
    main()
