#!/usr/bin/env python3
"""Render the generator gallery: one panel per generator case, on a contact sheet.

Consumes the JSON GeneratorGalleryTests writes. Knows nothing about generators -
a panel is a list of layers of filled polygons, lines and points - so a new test
case never needs a change here.
"""
import argparse, json, math
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

CELL = 460          # panel size in pixels
PAD = 26            # margin inside a panel, in pixels
LABEL = 34          # caption strip height
LEGEND_COLUMN = 112  # width of one legend entry, in pixels
LEGEND_ROW = 14      # height of one legend row, in pixels
LEGEND_PAD = 4       # gap between the caption and the legend


def _font(size):
    for name in ("/System/Library/Fonts/Supplemental/Arial.ttf",
                 "/System/Library/Fonts/Helvetica.ttc",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def bounds(panel):
    lo = [math.inf, math.inf]
    hi = [-math.inf, -math.inf]
    for layer in panel["layers"]:
        if layer["kind"] == "legend":
            continue
        for shape in layer["shapes"]:
            for x, y in shape:
                lo[0], lo[1] = min(lo[0], x), min(lo[1], y)
                hi[0], hi[1] = max(hi[0], x), max(hi[1], y)
    if lo[0] > hi[0]:
        return (0.0, 0.0, 1.0, 1.0)
    return (lo[0], lo[1], hi[0], hi[1])


def legend_rows(panel):
    """Swatch-and-name entries a panel asked to be labelled with.

    A legend layer carries its name in `note` and no geometry; the map itself
    cannot be labelled cell by cell at this size, so the colours are named once
    underneath it instead.
    """
    return [(l["colour"], l.get("note", "")) for l in panel["layers"] if l["kind"] == "legend"]


def legend_height(panel):
    rows = legend_rows(panel)
    if not rows:
        return 0
    per_row = max(1, (CELL - 12) // LEGEND_COLUMN)
    return LEGEND_PAD + ((len(rows) + per_row - 1) // per_row) * LEGEND_ROW


def render_panel(panel):
    """One panel, drawn to fit its own extent with the y axis pointing up."""
    strip = LABEL + legend_height(panel)
    image = Image.new("RGB", (CELL, CELL + strip), "#ffffff")
    draw = ImageDraw.Draw(image, "RGBA")

    x0, y0, x1, y1 = bounds(panel)
    span = max(x1 - x0, y1 - y0, 1e-9)
    scale = (CELL - 2 * PAD) / span
    # Centre the shorter axis, and flip y: the generators work in a right-handed
    # map frame and an image's y grows downward.
    ox = PAD + ((CELL - 2 * PAD) - (x1 - x0) * scale) / 2
    oy = PAD + ((CELL - 2 * PAD) - (y1 - y0) * scale) / 2

    def to_px(p):
        return (ox + (p[0] - x0) * scale, CELL - (oy + (p[1] - y0) * scale))

    for layer in panel["layers"]:
        kind, colour, width = layer["kind"], layer["colour"], layer["width"]
        if kind == "legend":
            continue
        for shape in layer["shapes"]:
            if len(shape) == 0:
                continue
            pts = [to_px(p) for p in shape]
            if kind == "fill" and len(pts) >= 3:
                draw.polygon(pts, fill=colour)
            elif kind == "hole" and len(pts) >= 3:
                # A hole is drawn in the panel background, so a courtyard reads
                # as a courtyard rather than as a differently coloured lot.
                draw.polygon(pts, fill="#ffffff")
            elif kind == "line" and len(pts) >= 2:
                draw.line(pts, fill=colour, width=max(1, int(round(width))), joint="curve")
            elif kind == "point":
                r = max(1.0, width)
                cx, cy = pts[0]
                draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=colour)

    draw.rectangle([0, 0, CELL - 1, CELL - 1], outline="#c8ccd2")
    draw.rectangle([0, CELL, CELL - 1, CELL + strip - 1], fill="#f4f5f7", outline="#c8ccd2")
    draw.text((8, CELL + 4), panel["name"], fill="#1d2b36", font=_font(14))
    draw.text((8, CELL + 19), panel["note"][:72], fill="#5a6672", font=_font(11))

    rows = legend_rows(panel)
    if rows:
        font = _font(10)
        per_row = max(1, (CELL - 12) // LEGEND_COLUMN)
        for i, (colour, name) in enumerate(rows):
            x = 8 + (i % per_row) * LEGEND_COLUMN
            y = CELL + LABEL + LEGEND_PAD + (i // per_row) * LEGEND_ROW
            draw.rectangle([x, y, x + 9, y + 9], fill=colour, outline="#9aa3ac")
            draw.text((x + 13, y - 1), name[:18], fill="#3b4752", font=font)

    return image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("json", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--columns", type=int, default=4)
    a = ap.parse_args()

    panels = json.loads(a.json.read_text())["panels"]
    cols = min(a.columns, max(1, len(panels)))
    rows = (len(panels) + cols - 1) // cols

    # A panel with a legend is taller, so each row is sized by its own tallest
    # panel rather than by a single constant.
    tiles = [render_panel(p) for p in panels]
    heights = [max(t.height for t in tiles[r * cols:(r + 1) * cols]) for r in range(rows)]

    sheet = Image.new("RGB", (cols * CELL, sum(heights)), "#ffffff")
    for i, tile in enumerate(tiles):
        sheet.paste(tile, ((i % cols) * CELL, sum(heights[:i // cols])))

    a.out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(a.out)
    print(f"{len(panels)} panels -> {a.out} ({sheet.width}x{sheet.height})")


if __name__ == "__main__":
    main()
