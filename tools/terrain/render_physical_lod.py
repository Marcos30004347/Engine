#!/usr/bin/env python3
"""Compare measured display depths and native captures; no invented terrain imagery."""
import json
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

root = Path(sys.argv[1])
report = json.loads((root / 'validation.json').read_text())
probes = json.loads((root / 'water-probes.json').read_text())
assert report['passed'] and report['waterline']['maximumDepthDelta'] == 0
assert report['waterline']['unsafeCoverageChanges'] > 0
font = ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc', 24)
small = ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc', 17)
im = Image.new('RGB', (1500, 1040), '#17232b')
d = ImageDraw.Draw(im)
d.text((28, 24), 'STEP 17b.1 / physical geometry preserved across display LODs', font=font, fill='#e8ebdf')
d.text((28, 67), 'Exact triangle-depth probes in the river footprint. Blue = water above ground; tan = exposed ground.', font=small, fill='#c8d4d0')
labels = ['Finest / reference', 'Protected LOD 1', 'Protected LOD 2', 'Unprotected coarse / rejected']
for column, label in enumerate(labels):
    ox = 28 + column * 370
    d.text((ox, 111), label, font=small, fill='#e8ebdf')
    # X runs across the channel; Z runs downstream. Stretched X makes its
    # thin 4-metre footprint legible; this is a classification chart, not a map.
    chart = Image.new('RGB', (16, 192))
    for x, z, *depths in probes:
        chart.putpixel((round((x-46.125)/.25), round((z-.125)/.25)), (70, 139, 170) if depths[column] > 0 else (189, 171, 126))
    im.paste(chart.resize((320, 400), Image.Resampling.NEAREST), (ox, 150))
    caption = '0 depth change' if column in (1, 2) else ('Reference' if column == 0 else f"{report['waterline']['unsafeCoverageChanges']} coverage changes")
    d.text((ox, 565), caption, font=small, fill='#c8d4d0')
for n in range(3):
    native = Image.open(root / f'lod-{n}-oblique.png')
    native.thumbnail((460, 310))
    x = 28 + n * 492
    im.paste(native, (x, 625))
    d.text((x, 928), f"LOD {n}: {report['lods'][n]['triangles']} triangles", font=small, fill='#e8ebdf')
d.text((28, 978), 'Native mesh captures below; diagnostic depth masks above. The synthetic crossing still lacks an engineered bridge.', font=small, fill='#c8d4d0')
im.save(root / '17-physical-lod.png')
print('PASS physical LOD diagnostic: exact protected depth masks; unprotected control exposes regression')
