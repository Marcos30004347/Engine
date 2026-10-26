#!/usr/bin/env python3
"""Inspect native block geometry with consistent before/after camera framing."""
import argparse
import copy
from pathlib import Path
from load_city_mesh import load_city_mesh
from render_city_model import render, OVERVIEW_MATERIALS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--title', default='STREET-FIRST BLOCK')
    parser.add_argument('--clay', action='store_true', help='Show geometry without textures')
    parser.add_argument('--gateway', action='store_true', help='Street-facing detail for the 46 x 38 block fixture')
    args = parser.parse_args()
    data = load_city_mesh(args.input)
    if args.clay:
        data = copy.copy(data)
        data.pop('textureManifest', None)
        data.pop('groundWeights', None)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.gateway:
        render(data, args.output, 3.14, 1.51, (11, -8, 35, 15), args.title,
               OVERVIEW_MATERIALS, width=1500, height=1050)
    else:
        render(data, args.output, -.72, .58, title=args.title,
               allowed=OVERVIEW_MATERIALS, width=1600, height=1150)


if __name__ == '__main__':
    main()
