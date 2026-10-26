"""Perspective interior inspection of native meshes; no cutaway or hidden roofs."""
import argparse
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from render_city_model import PALETTE, raster_triangle, shade
from dwelling_materials import Materials


def render_interior(data, path, eye, target, fov=78, width=1400, height=960, title="EYE LEVEL / NATIVE BUILDING MESH"):
    eye = np.array(eye, dtype=float)
    forward = np.array(target, dtype=float) - eye
    distance = np.linalg.norm(forward)
    if distance < 1e-8:
        raise ValueError("Camera eye and target must differ")
    forward /= distance
    right = np.cross(forward, [0., 1., 0.])
    if np.linalg.norm(right) < 1e-8:
        raise ValueError("Camera must not point vertically")
    right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    focal = width / (2 * math.tan(math.radians(fov) / 2))
    colour = np.full((height, width, 3), (204, 218, 225), dtype=np.uint8)
    depth = np.full((height, width), -np.inf, dtype=np.float32)
    vertices = np.array(data["vertices"], dtype=float)
    relative = vertices - eye
    # Element-wise reductions avoid platform BLAS floating-point warnings for
    # this very small (three-column) camera transform.
    camera = np.column_stack(tuple(np.sum(relative * axis, axis=1) for axis in (right, up, forward)))
    library = Materials(data)
    for ti, (triangle, material) in enumerate(zip(data["triangles"], data["materials"])):
        base, uv, texture = library.surface(data, ti, PALETTE.get(material, (183,170,145)))
        ground = library.ground(data, ti)
        if ground is not None:
            uv = np.asarray(data["triangleUVs"][ti], dtype=float)
        if uv is None:
            uv = np.zeros((3, 2))
        vertex_weights = ground[0] if ground is not None else np.zeros((3, 3))
        polygon = [np.concatenate((camera[i], t, w)) for i, t, w in zip(triangle, uv, vertex_weights)]
        clipped = []
        previous = polygon[-1]
        for current in polygon:
            dp, dc = previous[2] - .04, current[2] - .04
            if (dp >= 0) != (dc >= 0):
                clipped.append(previous + (current - previous) * (dp / (dp - dc)))
            if dc >= 0:
                clipped.append(current)
            previous = current
        if len(clipped) < 3:
            continue
        screen = [(width/2 + p[0]*focal/p[2], height/2 - p[1]*focal/p[2], 1/p[2])
                  for p in clipped]
        rgb = shade(base, vertices[triangle])
        light = shade((255,255,255), vertices[triangle])[0]/255
        if ground is not None:
            light = 1.0
        for i in range(2, len(screen)):
            clipped_ground = None if ground is None else (
                np.array([clipped[0][5:], clipped[i-1][5:], clipped[i][5:]]), ground[1], ground[2])
            raster_triangle(colour, depth, screen[0], screen[i-1], screen[i], rgb,
                            [clipped[0][3:5], clipped[i-1][3:5], clipped[i][3:5]], texture, light, True, clipped_ground)
    result = Image.fromarray(colour)
    draw = ImageDraw.Draw(result)
    draw.rectangle((18, 18, width-18, 60), fill=(233, 226, 212))
    draw.text((32, 33), title, fill=(47, 41, 36))
    path.parent.mkdir(parents=True, exist_ok=True)
    result.save(path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--eye", nargs=3, type=float, required=True)
    parser.add_argument("--target", nargs=3, type=float, required=True)
    parser.add_argument("--fov", type=float, default=78)
    args = parser.parse_args()
    if not 1 < args.fov < 175:
        parser.error("fov must be between 1 and 175 degrees")
    render_interior(json.loads(args.input.read_text()), args.output, args.eye, args.target, args.fov)


if __name__ == "__main__":
    main()
