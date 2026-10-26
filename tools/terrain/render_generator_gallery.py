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
        for shape in layer["shapes"]:
            for x, y in shape:
                lo[0], lo[1] = min(lo[0], x), min(lo[1], y)
                hi[0], hi[1] = max(hi[0], x), max(hi[1], y)
    if lo[0] > hi[0]:
        return (0.0, 0.0, 1.0, 1.0)
    return (lo[0], lo[1], hi[0], hi[1])


def render_panel(panel):
    """One panel, drawn to fit its own extent with the y axis pointing up."""
    image = Image.new("RGB", (CELL, CELL + LABEL), "#ffffff")
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
    draw.rectangle([0, CELL, CELL - 1, CELL + LABEL - 1], fill="#f4f5f7", outline="#c8ccd2")
    draw.text((8, CELL + 4), panel["name"], fill="#1d2b36", font=_font(14))
    draw.text((8, CELL + 19), panel["note"][:64], fill="#5a6672", font=_font(11))
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

    sheet = Image.new("RGB", (cols * CELL, rows * (CELL + LABEL)), "#ffffff")
    for i, panel in enumerate(panels):
        sheet.paste(render_panel(panel), ((i % cols) * CELL, (i // cols) * (CELL + LABEL)))

    a.out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(a.out)
    print(f"{len(panels)} panels -> {a.out} ({sheet.width}x{sheet.height})")


if __name__ == "__main__":
    main()
