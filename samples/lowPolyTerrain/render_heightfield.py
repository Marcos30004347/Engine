#!/usr/bin/env python3
"""Shade a --field dump for terrain QA (requires NumPy and Pillow).

Colors illustrate elevation; this is not a preview of the biome materials.
Usage: render_heightfield.py terrain.f32 --before original.npy --output comparison.png
"""
import argparse
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw


def read_field(path):
    if path.suffix == '.npy':
        return np.load(path)
    with path.open('rb') as stream:
        side = int(np.fromfile(stream, dtype='<u4', count=1)[0])
        return np.fromfile(stream, dtype='<f4', count=side * side).reshape(side, side)


def shade(height, extent, colors=None):
    dy, dx = np.gradient(height, extent / height.shape[0])
    light = np.clip((1 - .7 * dx - .5 * dy) / np.sqrt(1 + dx*dx + dy*dy) * .65 + .18, .12, 1.1)
    rgb = np.full((*height.shape, 3), [112., 153., 102.])
    rock = np.clip((height - 1200) / 1200, 0, 1)[..., None]
    rgb = rgb * (1 - rock) + np.array([160, 153, 132]) * rock
    snow = np.clip((height - 2300) / 500, 0, 1)[..., None]
    rgb = rgb * (1 - snow) + np.array([241, 245, 249]) * snow
    if colors is not None:
        rgb = np.asarray(Image.open(colors), dtype=float)
    rgb *= light[..., None]
    if colors is None:
        rgb[height <= 0] = [78, 139, 174]
    return Image.fromarray(np.uint8(np.clip(rgb[::-1], 0, 255)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('field', type=Path)
    parser.add_argument('--before', type=Path)
    parser.add_argument('--colors', type=Path, help='optional companion PPM with native material colors')
    parser.add_argument('--extent', type=float, default=128000, help='map extent in metres')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    fields = [(args.field, 'AFTER - reconstructed base, ridge graph, hydraulic erosion')]
    if args.before:
        fields.insert(0, (args.before, 'BEFORE - exported refined elevation'))
    result = Image.new('RGB', (800 * len(fields), 850), '#172027')
    draw = ImageDraw.Draw(result)
    for i, (path, title) in enumerate(fields):
        result.paste(shade(read_field(path), args.extent, args.colors if path == args.field else None).resize((800, 800)), (i*800, 50))
        draw.text((i*800 + 20, 20), title, fill='white')
    result.save(args.output)


if __name__ == '__main__':
    main()
