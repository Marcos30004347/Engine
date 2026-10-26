"""Inspect one native building mesh from six orthographic views and perspective."""
import argparse
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from load_city_mesh import load_city_mesh
from render_city_model import render, OVERVIEW_MATERIALS
from render_dwelling_eye_level import render_interior


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    data = load_city_mesh(args.input)
    views = [('top', 0, 0), ('front', math.pi, math.pi/2),
             ('back', 0, math.pi/2), ('left', math.pi/2, math.pi/2),
             ('right', -math.pi/2, math.pi/2), ('oblique', -.72, .65)]
    paths = []
    for name, yaw, pitch in views:
        path = args.output / (name+'.png')
        # Frame the actual vertices, including tall towers, rather than an
        # assumed building height or a ground-only crop.
        render(data, path, yaw, pitch, title=name.upper(),
               allowed=OVERVIEW_MATERIALS, width=1000, height=1000)
        paths.append(path)
    vertices = np.asarray(data['vertices'])
    low, high = vertices.min(axis=0), vertices.max(axis=0)
    centre = (low+high)/2
    radius = np.linalg.norm(high-low)*.5
    direction = np.array([-.85, .65, -1.])
    direction /= np.linalg.norm(direction)
    path = args.output/'perspective.png'
    render_interior(data, path, centre+direction*radius*3.2, centre,
                    fov=48, width=1200, height=1000, title='PERSPECTIVE / NATIVE BUILDING MESH')
    paths.append(path)
    sheet = Image.new('RGB', (1600, 800), (233, 226, 212))
    for i, path in enumerate(paths):
        thumb = Image.open(path)
        thumb.thumbnail((400, 400))
        sheet.paste(thumb, ((i%4)*400, (i//4)*400))
    ImageDraw.Draw(sheet).text((1220, 460), 'Native mesh inspection\nTop / front / back / left\nRight / oblique / perspective', fill=(47,41,36))
    sheet.save(args.output/'contact-sheet.png')


if __name__ == '__main__':
    main()
