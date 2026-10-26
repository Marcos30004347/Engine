#!/usr/bin/env python3
"""Draw the dwelling gallery's blueprint panels as a contact sheet.

The gallery writes two documents. `dwelling-gallery-solids.json` is geometry in
three dimensions and `render_dwelling_solids.py` draws it; this one is the
*plan* - rooms by purpose, doors, windows, stairs, furniture - as flat layers of
shapes, and until now nothing drew it at all, so the only way to look at a floor
plan was to infer it from a three dimensional render of the walls.

A panel is a list of layers and a layer is a colour, a width, a kind and some
shapes in metres. There is nothing in the document about how big to draw it:
each panel is fitted to its own cell, so a cottage and a manor are drawn at
whatever scale shows them, and the scale is written under each one.
"""

import argparse
import json
import math

from PIL import Image, ImageDraw, ImageFont


PAPER = (247, 245, 240)
INK = (60, 62, 68)
CELL = 460
PAD = 26
LABEL = 30


def font(size):
    for name in ("/System/Library/Fonts/Helvetica.ttc", "/Library/Fonts/Arial.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue

    return ImageFont.load_default()


def bounds(panel):
    """The extent of everything drawn in a panel, in metres."""
    lo = [math.inf, math.inf]
    hi = [-math.inf, -math.inf]

    for layer in panel.get("layers", ()):
        for shape in layer.get("shapes", ()):
            for point in shape:
                lo[0] = min(lo[0], point[0])
                lo[1] = min(lo[1], point[1])
                hi[0] = max(hi[0], point[0])
                hi[1] = max(hi[1], point[1])

    if lo[0] > hi[0]:
        return None

    return lo, hi


def colour(text):
    text = (text or "#404040").lstrip("#")

    return tuple(int(text[i:i + 2], 16) for i in (0, 2, 4))


def draw(panel, image, ox, oy, side):
    """One panel, fitted to a `side` by `side` cell whose corner is (ox, oy)."""
    extent = bounds(panel)
    pen = ImageDraw.Draw(image, "RGBA")

    if extent is None:
        return None

    lo, hi = extent
    span = max(hi[0] - lo[0], hi[1] - lo[1], 1e-6)
    scale = (side - PAD * 2) / span
    # Centred in the cell, and with Y down the page: a plan is read from the
    # north, which is the opposite of the way an image is indexed.
    dx = ox + PAD + ((side - PAD * 2) - (hi[0] - lo[0]) * scale) * .5
    dy = oy + PAD + ((side - PAD * 2) - (hi[1] - lo[1]) * scale) * .5

    def at(point):
        return (dx + (point[0] - lo[0]) * scale, dy + ((hi[1] - lo[1]) - (point[1] - lo[1])) * scale)

    for layer in panel.get("layers", ()):
        kind = layer.get("kind")
        tint = colour(layer.get("colour"))
        width = max(1, int(round(float(layer.get("width", 1)) * scale / 12)))

        for shape in layer.get("shapes", ()):
            if not shape:
                continue

            points = [at(p) for p in shape]

            if kind == "fill" and len(points) >= 3:
                # Translucent, so a room's colour reads under the walls and the
                # furniture drawn over it rather than hiding them.
                pen.polygon(points, fill=tint + (205,))
            elif kind == "point":
                r = max(2, float(layer.get("width", 2)))
                pen.ellipse([points[0][0] - r, points[0][1] - r, points[0][0] + r, points[0][1] + r], fill=tint)
            elif len(points) >= 2:
                pen.line(points, fill=tint, width=max(1, width), joint="curve")

    return scale


def main():
    parse = argparse.ArgumentParser()
    parse.add_argument("gallery")
    parse.add_argument("out")
    parse.add_argument("--columns", type=int, default=4)
    parse.add_argument("--match", default="", help="only panels whose name contains this")
    parse.add_argument("--limit", type=int, default=0)
    args = parse.parse_args()

    with open(args.gallery) as handle:
        document = json.load(handle)

    panels = [p for p in document.get("panels", ()) if args.match in p.get("name", "")]

    if args.limit:
        panels = panels[:args.limit]

    if not panels:
        print("no panels matched")

        return

    columns = max(1, min(args.columns, len(panels)))
    rows = (len(panels) + columns - 1) // columns
    image = Image.new("RGB", (columns * CELL, rows * (CELL + LABEL)), PAPER)
    pen = ImageDraw.Draw(image)
    small = font(13)
    tiny = font(11)

    for i, panel in enumerate(panels):
        ox = (i % columns) * CELL
        oy = (i // columns) * (CELL + LABEL)
        scale = draw(panel, image, ox, oy, CELL)

        pen.text((ox + 8, oy + CELL + 4), panel.get("name", "?"), font=small, fill=INK)

        if scale:
            pen.text((ox + CELL - 78, oy + CELL + 5), "1 m = %.0f px" % scale, font=tiny, fill=(140, 140, 146))

        note = (panel.get("note") or "").strip()

        if note:
            pen.text((ox + 8, oy + 8), note[:78], font=tiny, fill=(130, 132, 140))

        if i % columns:
            pen.line([(ox, oy), (ox, oy + CELL + LABEL)], fill=(226, 224, 218))

        if i >= columns:
            pen.line([(ox, oy), (ox + CELL, oy)], fill=(226, 224, 218))

    image.save(args.out)
    print("%d panels -> %s (%dx%d)" % (len(panels), args.out, image.width, image.height))


if __name__ == "__main__":
    main()
