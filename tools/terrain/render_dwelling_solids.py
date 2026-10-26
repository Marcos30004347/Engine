#!/usr/bin/env python3
"""Draw the exterior of each planned building as a shaded solid.

A floor plan says nothing about whether a building stands up, and a roof drawn
as a ridge line over a plan hides the two faults that matter: a roof that does
not reach its walls, and wings that interpenetrate instead of meeting. So this
projects the wall and roof polygons the planner emits and rasterises them.

Deliberately not a renderer: axonometric projection, one directional light,
flat shading. Enough to see the massing and nothing that could flatter it.

It does use a real z-buffer rather than a painter's sort, because a painter's
sort cannot draw these buildings. Sorting faces by depth assumes no face can
be partly in front of and partly behind another, and a roof that oversails its
own walls breaks that on every building: a large near roof was drawn over the
walls of a small wing standing behind it, so a tower appeared as a roof
floating in the air with nothing underneath. Per-pixel depth has no such
assumption and the artifact class disappears entirely.
"""

import argparse
import json
import math

import numpy as np
from PIL import Image, ImageDraw, ImageFont

# The view. A half-turn plus a tilt, so no face of an axis-aligned building is
# seen edge on and the roof reads as a roof.
#
# PITCH is measured from plan: at zero the projection is a floor plan, at a
# right angle it is a pure elevation. Low values look nearly straight down, and
# there a storey of wall compresses to a couple of pixels while the roof keeps
# its full width - which reads as a building with no walls at all.
YAW = math.radians(35)
PITCH = math.radians(50)

LIGHT = (-0.45, 0.78, -0.44)

PALETTE = {
    "garden": (150, 168, 118),
    "yard": (201, 193, 168),
    "walls": (214, 203, 182),
    "innerwalls": (196, 186, 166),
    "pillars": (176, 166, 148),
    "slopes": (150, 82, 62),
    "soffits": (110, 62, 48),
    "gables": (196, 184, 162),
}

# Ground first, so the flat pieces do not steal the depth test from the things
# standing on them when the two touch.
KINDS = ("garden", "yard", "walls", "innerwalls", "pillars", "slopes", "soffits", "gables")

BACKGROUND = (247, 246, 243)
EDGE = (64, 57, 50)


def project(p):
    """World to (screen x, screen y, depth).

    Depth increases *towards* the viewer: the camera is above the building, so
    a point higher up is nearer to it. Screen height rises with y and with
    distance away, and `ez` grows towards the camera, so both terms of the
    second component are negated against the obvious spelling.
    """
    x, y, z = p
    cx, sx = math.cos(YAW), math.sin(YAW)
    ex, ez = x * cx - z * sx, x * sx + z * cx
    cy, sy = math.cos(PITCH), math.sin(PITCH)

    return ex, y * sy - ez * cy, ez * sy + y * cy


def normal(face):
    # Newell's method: correct for any planar polygon, and it does not care
    # which three vertices you happen to pick.
    nx = ny = nz = 0.0

    for i, a in enumerate(face):
        b = face[(i + 1) % len(face)]
        nx += (a[1] - b[1]) * (a[2] + b[2])
        ny += (a[2] - b[2]) * (a[0] + b[0])
        nz += (a[0] - b[0]) * (a[1] + b[1])

    length = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0

    return nx / length, ny / length, nz / length


def lighting(face):
    """A normal to shade by, with no claim about which side is out.

    Orienting a wall "away from the middle of the building" was wrong, and
    wrong in a way that only a stepped building shows: a face on an inward
    notch - the side of a tower where it meets a lower wing - points the
    opposite way from the building's middle, so it was flipped, judged to face
    away from the camera and dropped. A three-storey tower appeared as a roof
    floating with nothing underneath it, and the geometry was correct all
    along.

    With a depth buffer there is nothing to decide. Every face is drawn; a
    face that should not be visible is simply overwritten by the one in front
    of it, which is the job the buffer exists to do. Shading takes the
    magnitude of the light term, so a surface is lit the same whichever way
    its winding happens to run.
    """
    return normal(face)


def shade(base, n):
    lit = abs(n[0] * LIGHT[0] + n[1] * LIGHT[1] + n[2] * LIGHT[2])
    k = 0.40 + 0.60 * lit

    return tuple(min(255, int(c * k)) for c in base)


def gather(buildings):
    """Every face of every building, projected, with its shade.

    Nothing is culled. The depth buffer decides what is seen, and it is the
    only thing here that can: a hidden face is overwritten by whatever stands
    in front of it.
    """
    out = []

    for building in buildings:
        for kind in KINDS:
            for face in building.get(kind, []):
                if len(face) >= 3:
                    # Ground is unshaded: it is flat, so a light term would
                    # give every garden in the town the same flat value anyway,
                    # and a lit one reads as a surface rather than as ground.
                    rgb = PALETTE[kind] if kind in ("garden", "yard") else shade(PALETTE[kind], lighting(face))

                    # Roof faces are not outlined.
                    #
                    # The roof is a sampled wavefront, so one plane arrives as
                    # a few dozen triangles and stroking each of them draws the
                    # sampling itself: every slope came out ruled with contour
                    # lines. The creases that matter - ridges, hips, valleys -
                    # separate planes of different normals and so of different
                    # shades, and show without any help.
                    out.append(([project(v) for v in face], rgb, kind not in ("slopes", "soffits", "gables")))

    return out


def rasterise(faces, width, height, pad):
    """Z-buffered fan triangulation of every face.

    The alternative, sorting faces by depth and drawing back to front, cannot
    order a roof that oversails the wall of the wing behind it - the two
    interleave in depth - and that showed up as walls vanishing under a
    neighbour's roof. A depth per pixel has nothing to order.
    """
    if not faces:
        return None

    xs = [p[0] for f, _, _ in faces for p in f]
    ys = [p[1] for f, _, _ in faces for p in f]
    scale = min((width - 2 * pad) / (max(xs) - min(xs) or 1), (height - 2 * pad) / (max(ys) - min(ys) or 1))
    ox, oy = min(xs), min(ys)

    colour = np.zeros((height, width, 3), dtype=np.float32)
    colour[:] = BACKGROUND
    depth = np.full((height, width), -np.inf, dtype=np.float32)

    # Screen-space triangles, plus the edges to stroke afterwards.
    strokes = []

    for face, rgb, outlined in faces:
        flat = [(pad + (p[0] - ox) * scale, height - pad - (p[1] - oy) * scale, p[2]) for p in face]

        if outlined:
            strokes.append(flat)

        for k in range(1, len(flat) - 1):
            triangle(colour, depth, flat[0], flat[k], flat[k + 1], rgb)

    image = Image.fromarray(np.clip(colour, 0, 255).astype(np.uint8), "RGB")
    drawer = ImageDraw.Draw(image)

    # Outlines, drawn only where the face they belong to is the nearest thing
    # on that pixel, so a hidden edge does not draw through the solid in front
    # of it. Sampled at the midpoint of each edge, which is enough for the
    # straight edges these buildings have.
    for flat in strokes:
        for i, a in enumerate(flat):
            b = flat[(i + 1) % len(flat)]
            mx, my = (a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5
            px, py = int(round(mx)), int(round(my))

            if not (0 <= px < width and 0 <= py < height):
                continue

            if depth[py, px] <= (a[2] + b[2]) * 0.5 + 1e-3:
                drawer.line([(a[0], a[1]), (b[0], b[1])], fill=EDGE, width=1)

    return image


def triangle(colour, depth, a, b, c, rgb):
    """One z-buffered triangle, over its own bounding box only."""
    height, width = depth.shape

    minx = max(0, int(math.floor(min(a[0], b[0], c[0]))))
    maxx = min(width - 1, int(math.ceil(max(a[0], b[0], c[0]))))
    miny = max(0, int(math.floor(min(a[1], b[1], c[1]))))
    maxy = min(height - 1, int(math.ceil(max(a[1], b[1], c[1]))))

    if minx > maxx or miny > maxy:
        return

    area = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])

    if abs(area) < 1e-9:
        return

    xs = np.arange(minx, maxx + 1, dtype=np.float32)[None, :] + 0.5
    ys = np.arange(miny, maxy + 1, dtype=np.float32)[:, None] + 0.5

    w0 = ((b[0] - a[0]) * (ys - a[1]) - (xs - a[0]) * (b[1] - a[1])) / area
    w1 = ((xs - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (ys - a[1])) / area
    w2 = 1.0 - w0 - w1

    # A hair outside counts as inside.
    #
    # Two triangles sharing an edge both own the pixels on it, so in exact
    # arithmetic there is no seam. In floating point a pixel centre a
    # ten-millionth off the line can fall outside both, and the background
    # shows through as a one-pixel crack - which is what the slits across every
    # hip and valley were. The z-buffer settles any overlap the tolerance
    # creates.
    inside = (w0 >= -1e-4) & (w1 >= -1e-4) & (w2 >= -1e-4)

    if not inside.any():
        return

    z = w2 * a[2] + w1 * b[2] + w0 * c[2]

    window = depth[miny : maxy + 1, minx : maxx + 1]
    nearer = inside & (z > window)

    if not nearer.any():
        return

    window[nearer] = z[nearer]
    colour[miny : maxy + 1, minx : maxx + 1][nearer] = rgb


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("solids")
    parser.add_argument("out")
    parser.add_argument("--size", type=int, default=420)
    parser.add_argument("--scene", action="store_true", help="draw every building in one view")
    parser.add_argument("--width", type=int, default=1800)
    parser.add_argument("--height", type=int, default=1200)
    a = parser.parse_args()

    document = json.load(open(a.solids))
    buildings = document["buildings"]

    if a.scene:
        # A town has to be drawn as a town: buildings one to a tile cannot show
        # whether they crowd a street or stand off it.
        image = rasterise(gather(buildings), a.width, a.height, 24)

        if image is None:
            raise SystemExit("no buildings with faces")

        image.save(a.out)
        print(f"{len(buildings)} buildings -> {a.out} ({image.width}x{image.height})")

        return

    tiles = [(b["name"], rasterise(gather([b]), a.size, a.size, 26)) for b in buildings]
    tiles = [(name, image) for name, image in tiles if image is not None]

    if not tiles:
        raise SystemExit("no buildings with faces")

    columns = min(4, len(tiles))
    rows = (len(tiles) + columns - 1) // columns
    label = 22

    sheet = Image.new("RGB", (columns * a.size, rows * (a.size + label)), (255, 255, 255))
    drawer = ImageDraw.Draw(sheet)

    try:
        font = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 13)
    except OSError:
        font = ImageFont.load_default()

    for i, (name, image) in enumerate(tiles):
        x, y = (i % columns) * a.size, (i // columns) * (a.size + label)
        sheet.paste(image, (x, y))
        drawer.rectangle([x, y + a.size, x + a.size, y + a.size + label], fill=(244, 244, 246))
        drawer.text((x + 6, y + a.size + 4), name, fill=(40, 40, 44), font=font)
        drawer.rectangle([x, y, x + a.size - 1, y + a.size + label - 1], outline=(214, 214, 218))

    sheet.save(a.out)
    print(f"{len(tiles)} buildings -> {a.out} ({sheet.width}x{sheet.height})")


if __name__ == "__main__":
    main()
