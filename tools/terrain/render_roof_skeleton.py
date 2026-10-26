#!/usr/bin/env python3
"""Draw a roof as its structure rather than as a picture of itself.

The shaded render cannot settle an argument about a roof. A fold and a hole
shade almost the same, a ridge and a seam shade exactly the same, and a
surface that terraces reads as torn even though it covers its plan. Every
roof question this project has actually had - is that a missing face, is that
one plane or three, is the ridge an edge or a row of points - is a question
about the structure, and shading hides structure by construction.

So this draws no light at all. Three panels per roof:

  plan        Straight down. No occlusion, so nothing can hide behind
              anything; every face is visible at its true plan size.
  skeleton    The same view as the solid render, wireframe, no fill: every
              edge of every face, including the ones inside the surface.
  planes      The same view, each *plane* flat-filled in its own colour and
              nothing else. A hip is four colours, a terraced mess is thirty.

and on the plan panel, in red, the fault that matters: **plan with no roof over
it**. A roof that covers exactly its own area can still be torn, because a fold
somewhere cancels a shortfall somewhere else; and a hole of a fifth of a square
metre is invisible to every summary number a roof has. So the plan is rasterised
and the uncovered cells that are enclosed by roof on all sides are painted.

Tried first as a question about edges, which is the elegant form: every edge of
a closed surface is shared, so an unshared edge inside the plan is a hole. It
does not survive contact with this surface. The faces do not share vertices - a
merged plane meets a row of strip triangles - so the pairing is per line rather
than per vertex, and two edges a tenth of a millimetre apart are the same line
to the roof and different lines to any keying of it. Every tolerance that
stopped it inventing slits started missing real ones. Counting cells is dull
and cannot be argued with.
"""

import argparse
import colorsys
import json
import math
from collections import deque

import numpy as np

from PIL import Image, ImageDraw, ImageFont

YAW = math.radians(35)
PITCH = math.radians(50)

BACKGROUND = (250, 249, 246)
INK = (48, 44, 40)
SEAM = (150, 146, 140)
EAVES = (40, 70, 140)
HOLE = (214, 40, 40)
GABLE_FILL = (196, 190, 178)


def project(p):
    x, y, z = p
    cx, sx = math.cos(YAW), math.sin(YAW)
    ex, ez = x * cx - z * sx, x * sx + z * cx
    cy, sy = math.cos(PITCH), math.sin(PITCH)

    return ex, y * sy - ez * cy


def plan(p):
    return p[0], -p[2]


def normal(face):
    nx = ny = nz = 0.0

    for i, a in enumerate(face):
        b = face[(i + 1) % len(face)]
        nx += (a[1] - b[1]) * (a[2] + b[2])
        ny += (a[2] - b[2]) * (a[0] + b[0])
        nz += (a[0] - b[0]) * (a[1] + b[1])

    length = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0

    return nx / length, ny / length, nz / length


def plane_key(face):
    """A plane, to within a tenth of a degree and a centimetre.

    Signed the same way whichever way the face is wound, so two triangles of
    one plane never land in two groups because their winding disagrees.
    """
    nx, ny, nz = normal(face)

    if ny < 0 or (abs(ny) < 1e-9 and (nx < 0 or (abs(nx) < 1e-9 and nz < 0))):
        nx, ny, nz = -nx, -ny, -nz

    d = nx * face[0][0] + ny * face[0][1] + nz * face[0][2]

    return (round(nx, 3), round(ny, 3), round(nz, 3), round(d, 2))


def uncovered(faces, size=520):
    """Plan the roof does not cover, as a mask and an area.

    Enclosed only: a cell with no roof over it that can walk to the edge of the
    picture is outside the building, not a hole in it.

    A courtyard reads as a hole here, and from the roof alone it is one - the
    faces are all this has, and they say nothing about whether the gap in the
    middle is a court the building is wrapped around or a tear. Read the plan
    panel: a court is bounded by eaves all the way round and is the size of a
    room, a tear is a sliver along a ridge.
    """
    points = [(v[0], v[2]) for f in faces for v in f]
    lo = (min(p[0] for p in points), min(p[1] for p in points))
    hi = (max(p[0] for p in points), max(p[1] for p in points))
    xs = np.linspace(lo[0], hi[0], size)
    zs = np.linspace(lo[1], hi[1], size)
    X, Z = np.meshgrid(xs, zs)
    covered = np.zeros((size, size), dtype=bool)

    for face in faces:
        ring = [(v[0], v[2]) for v in face]
        inside = np.zeros_like(X, dtype=bool)

        for i, (x0, z0) in enumerate(ring):
            x1, z1 = ring[(i + 1) % len(ring)]

            if z1 == z0:
                continue

            inside ^= ((z0 > Z) != (z1 > Z)) & (X < (x1 - x0) * (Z - z0) / (z1 - z0) + x0)

        covered |= inside

    bare = ~covered
    outside = np.zeros_like(bare)
    queue = deque()

    for i in range(size):
        for p in ((0, i), (size - 1, i), (i, 0), (i, size - 1)):
            if bare[p] and not outside[p]:
                outside[p] = True
                queue.append(p)

    while queue:
        y, x = queue.popleft()

        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            ny, nx = y + dy, x + dx

            if 0 <= ny < size and 0 <= nx < size and bare[ny, nx] and not outside[ny, nx]:
                outside[ny, nx] = True
                queue.append((ny, nx))

    holed = bare & ~outside

    # One cell of erosion, because a cell centre landing exactly on the
    # diagonal between two triangles is inside both in exact arithmetic and
    # inside neither once the cross products have rounded. Undressed, that
    # draws a dotted red line up the hip of every plain rectangle in the town.
    # A hole worth seeing is thicker than one cell; a seam never is.
    thick = holed.copy()
    thick[1:, :] &= holed[:-1, :]
    thick[:-1, :] &= holed[1:, :]
    thick[:, 1:] &= holed[:, :-1]
    thick[:, :-1] &= holed[:, 1:]

    # Grown back, so what is painted is the hole and not its middle.
    holed = thick.copy()
    holed[1:, :] |= thick[:-1, :]
    holed[:-1, :] |= thick[1:, :]
    holed[:, 1:] |= thick[:, :-1]
    holed[:, :-1] |= thick[:, 1:]

    cell = (hi[0] - lo[0]) * (hi[1] - lo[1]) / size / size

    return holed, holed.sum() * cell, lo, hi


def colours(n):
    return [tuple(int(255 * c) for c in colorsys.hls_to_rgb((i * 0.61803398875) % 1.0, 0.62, 0.52)) for i in range(max(n, 1))]


def draw(building, size, pad=26):
    slopes = building.get("slopes", [])
    gables = building.get("gables", [])
    faces = [f for f in slopes + gables if len(f) >= 3]

    if not faces:
        return None

    holed, torn, glo, ghi = uncovered(faces)

    groups = {}

    for face in faces:
        groups.setdefault(plane_key(face), []).append(face)

    order = sorted(groups, key=lambda k: -len(groups[k]))
    palette = dict(zip(order, colours(len(order))))

    panels = []

    for title, cast, filled in (("plan", plan, True), ("skeleton", project, False), ("planes", project, True)):
        points = [cast(v) for f in faces for v in f]
        lo = (min(p[0] for p in points), min(p[1] for p in points))
        hi = (max(p[0] for p in points), max(p[1] for p in points))
        span = max(hi[0] - lo[0], hi[1] - lo[1]) or 1.0
        scale = (size - 2 * pad) / span

        def place(p):
            q = cast(p)

            return (pad + (q[0] - lo[0]) * scale, pad + (q[1] - lo[1]) * scale)

        image = Image.new("RGB", (size, size), BACKGROUND)
        pen = ImageDraw.Draw(image)

        if filled:
            # Far to near, so the near faces of the axonometric view win. In
            # plan there is nothing to occlude and the order is irrelevant.
            for face in sorted(faces, key=lambda f: sum(v[1] for v in f) / len(f)):
                fill = GABLE_FILL if face in gables else palette[plane_key(face)]
                pen.polygon([place(v) for v in face], fill=fill)

        for face in faces:
            pen.polygon([place(v) for v in face], outline=SEAM if filled else INK)

        if title == "plan" and torn > 1e-4:
            rows, cols = np.nonzero(holed)
            n = holed.shape[0]

            for r, c in zip(rows, cols):
                x = glo[0] + (ghi[0] - glo[0]) * (c + .5) / n
                z = glo[1] + (ghi[1] - glo[1]) * (r + .5) / n
                sx, sy = place((x, 0, z))
                pen.rectangle([sx - 1.5, sy - 1.5, sx + 1.5, sy + 1.5], fill=HOLE)

        pen.text((6, size - 14), title, fill=INK)
        panels.append(image)

    sheet = Image.new("RGB", (size * len(panels), size + 18), BACKGROUND)

    for i, panel in enumerate(panels):
        sheet.paste(panel, (i * size, 0))

    label = f"{building.get('name', '?')}  {len(faces)} faces, {len(order)} planes, {torn:.3f} m2 of plan with no roof over it"
    ImageDraw.Draw(sheet).text((6, size + 3), label, fill=HOLE if torn > 1e-3 else INK)

    return sheet, torn, len(order)


def main():
    a = argparse.ArgumentParser()
    a.add_argument("solids")
    a.add_argument("out")
    a.add_argument("--size", type=int, default=320)
    a.add_argument("--only", default=None, help="comma separated names")
    a.add_argument("--holes", action="store_true", help="only roofs with a hole")
    a = a.parse_args()

    document = json.load(open(a.solids))
    buildings = document["buildings"]

    if a.only:
        wanted = set(a.only.split(","))
        buildings = [b for b in buildings if b.get("name") in wanted]

    sheets = []
    torn = 0

    for building in buildings:
        drawn = draw(building, a.size)

        if drawn is None:
            continue

        sheet, bare, planes = drawn
        torn += 1 if bare > 1e-3 else 0

        if bare > 1e-3:
            print(f"  {building.get('name','?'):18s} {bare:7.3f} m2 with no roof over it, {planes} planes")

        if bare > 1e-3 or not a.holes:
            sheets.append(sheet)

    if not sheets:
        raise SystemExit("nothing to draw")

    width = max(s.width for s in sheets)
    out = Image.new("RGB", (width, sum(s.height for s in sheets)), BACKGROUND)
    y = 0

    for sheet in sheets:
        out.paste(sheet, (0, y))
        y += sheet.height

    out.save(a.out)
    print(f"{len(sheets)} roofs -> {a.out} ({out.width}x{out.height}); {torn} of {len(buildings)} have a hole")


if __name__ == "__main__":
    main()
