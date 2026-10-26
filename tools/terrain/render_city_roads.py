#!/usr/bin/env python3
"""Compare authored city road geometry; no visual-only smoothing is applied."""
import argparse
import json
from pathlib import Path

from PIL import Image, ImageDraw


def render(before, after, output):
    points = [p for data in (before, after) for b in data['blocks'] for p in b['boundary']]
    x0, x1 = min(p[0] for p in points), max(p[0] for p in points)
    z0, z1 = min(p[2] for p in points), max(p[2] for p in points)
    scale = min(1440 / max(1, x1-x0), 1400 / max(1, z1-z0))
    canvas = Image.new('RGB', (3200, 1660), '#eee8da')
    for column, (data, title) in enumerate(((before, 'BEFORE'), (after, 'PLANNED STREETS + COMMERCIAL BLOCKS'))):
        panel = Image.new('RGB', (1600, 1660), '#eee8da')
        draw = ImageDraw.Draw(panel)
        def project(p):
            return (80 + (p[0]-x0)*scale, 130 + (p[2]-z0)*scale)
        for lot in data['lots']:
            allocation=lot.get('allocation', {})
            fill='#b69268' if allocation.get('building')=='shop-house' else '#d4cdbb'
            if allocation.get('residence') in (3,4): fill='#b27063'
            draw.polygon([project(p) for p in lot['boundary']], fill=fill, outline='#c0b7a4')
        for route in data['routes']:
            path = [project(p) for p in route['path']]
            if len(path) < 2:
                continue
            width = max(2, round(route['width']*scale))
            draw.line(path, fill='#686153', width=width+2, joint='curve')
            draw.line(path, fill='#fff9e9', width=width, joint='curve')
        for wall in data['wallSegments']:
            if wall['built']:
                draw.line([project(wall['a']), project(wall['b'])], fill='#423e36', width=5)
        draw.rectangle((0, 0, 1600, 85), fill='#eee8da')
        draw.text((60, 38), title, fill='#29251e', font_size=28)
        draw.text((60, 1600), 'Same seed / brown: merchant frontage / red: inns and pubs', fill='#50483c', font_size=23)
        canvas.paste(panel, (column*1600, 0))
    output.parent.mkdir(parents=True, exist_ok=True)
    canvas.resize((1600, 830), Image.Resampling.LANCZOS).save(output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    render(json.loads(args.before.read_text()), json.loads(args.after.read_text()), args.output)
