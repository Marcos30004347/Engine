#!/usr/bin/env python3
"""Render a camera crop of native city geometry with an occlusion margin."""
import argparse
import json
from pathlib import Path

from load_city_mesh import load_city_mesh
from render_city_model import OVERVIEW_MATERIALS, render


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    framing = parser.add_mutually_exclusive_group(required=True)
    framing.add_argument('--crop', type=float, nargs=4,
                        metavar=('X0', 'Z0', 'X1', 'Z1'))
    framing.add_argument('--whole-city', action='store_true')
    parser.add_argument('--width', type=int, default=1500)
    parser.add_argument('--height', type=int, default=1100)
    parser.add_argument('--margin', type=float, default=30)
    parser.add_argument('--yaw', type=float, default=-.72)
    parser.add_argument('--pitch', type=float, default=.88)
    parser.add_argument('--title', default='CITY GENERATOR / STONE COURT AND STREET FRONTAGES')
    args = parser.parse_args()
    frame_points = None
    if args.whole_city:
        document = json.loads(args.input.read_text())
        points = [p for building in document['buildings'] for p in building['footprint']]
        points += document.get('previewBounds', [])
        if not points:
            parser.error('whole-city framing requires building footprints')
        args.crop = (min(p[0] for p in points)-15, min(p[2] for p in points)-15,
                     max(p[0] for p in points)+15, max(p[2] for p in points)+15)
        # Actual occupied extents avoid framing empty corners of a rotated
        # axis-aligned box around a roughly circular walled city.
        frame_points = points + [(p[0], p[1]+b.get('wallHeight',0)+b.get('roofHeight',0), p[2])
                                 for b in document['buildings'] for p in b['footprint']]
    x0, z0, x1, z1 = args.crop
    if x1 <= x0 or z1 <= z0 or args.margin < 0:
        parser.error('crop must have positive width/depth and margin must be nonnegative')
    margin = args.margin
    data = load_city_mesh(args.input, (x0-margin, z0-margin, x1+margin, z1+margin))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    render(data, args.output, args.yaw, args.pitch, args.crop, args.title,
           OVERVIEW_MATERIALS, width=args.width, height=args.height, frame_points=frame_points)


if __name__ == '__main__':
    main()
