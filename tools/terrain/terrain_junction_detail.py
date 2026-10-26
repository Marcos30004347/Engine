"""Shared junction displacement and visual junction masks."""

import numpy as np
import shapely as sh
from PIL import Image, ImageDraw, ImageFilter
from terrain_noise_mountains import gradient_noise
from terrain_landforms import smooth


def warp_points(points, land, seed):
    p = np.asarray(points, float)
    coast = sh.distance(sh.points(p), land.boundary)
    # One world-space field warps every incident edge identically. The coast
    # remains fixed, and detailed ownership is validated after reconstruction.
    fade = smooth(coast / 1800)
    dx = 800 * gradient_noise(p[:, 0], p[:, 1], 4000, seed + 820)
    dz = 800 * gradient_noise(p[:, 0], p[:, 1], 4000, seed + 821)
    return p + np.column_stack([dx, dz]) * fade[:, None]


def paint_boundaries(base, product, xy, pixels_per_metre, colour="#657564", width=2):
    """Hide junction pinpoints with a local colour blend and faded line mask.

    This presentation mask does not punch holes in the ownership polygons.
    """
    vertices = [c["displayPoint"] for c in product["corners"] if not c["coast"]]
    if not vertices:
        return base
    centres = [xy(v) for v in vertices]
    junctions = Image.new("L", base.size)
    d = ImageDraw.Draw(junctions)
    radius = 120 * pixels_per_metre
    for x, y in centres:
        if -radius <= x <= base.width + radius and -radius <= y <= base.height + radius:
            d.ellipse((x - radius, y - radius, x + radius, y + radius), fill=255)
    junctions = junctions.filter(
        ImageFilter.GaussianBlur(max(1, 40 * pixels_per_metre))
    )
    result = Image.composite(
        base.filter(ImageFilter.GaussianBlur(max(1, 55 * pixels_per_metre))),
        base,
        junctions,
    )
    strokes = Image.new("L", base.size)
    sd = ImageDraw.Draw(strokes)
    for edge in product["edges"]:
        if len(edge["regions"]) == 2:
            sd.line(
                [xy(p) for p in edge["points"]], fill=255, width=width, joint="curve"
            )
    from PIL import ImageChops

    strokes = ImageChops.multiply(strokes, ImageChops.invert(junctions))
    return Image.composite(Image.new("RGB", base.size, colour), result, strokes)
