#!/usr/bin/env python3
"""Draws an access model: surfaces, the holes in them, the ramps between them.

Deliberately plain. The point of the model is that it is planes, and the point
of the drawing is to let you see the planes - so a surface is a filled polygon,
a hole is the background showing through, a ramp is a quad, and a door is a
dot with a stub for the way you face. Two views: the plan, and an elevation
along the wall, which is the one that shows a Z as a Z.
"""

import argparse
import json
import math

from PIL import Image, ImageDraw


GROUND = (203, 197, 182)
BALCONY = (214, 176, 118)
STOREY = (196, 190, 178)
LANDING = (200, 182, 150)
RAMP = (140, 92, 60)
DOOR = (31, 111, 63)
FAULT = (192, 32, 32)
INK = (60, 56, 50)
PAPER = (247, 245, 240)

SOLID = (170, 164, 152)
BARRIER = (92, 74, 58)

KINDS = {"ground": GROUND, "balcony": BALCONY, "storey": STOREY, "landing": LANDING}


def bounds(model):
    xs, ys, zs = [], [], []

    for surface in model["surfaces"]:
        for ring in [surface["outer"]] + surface["holes"]:
            for p in ring:
                xs.append(p[0])
                ys.append(p[1])
                zs.append(p[2])

    for ramp in model["ramps"]:
        for p in ramp["quad"]:
            xs.append(p[0])
            ys.append(p[1])
            zs.append(p[2])

    return min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)


def draw(model, path, scale, margin):
    lowX, highX, lowY, highY, lowZ, highZ = bounds(model)

    spanX = max(highX - lowX, 1.0)
    spanZ = max(highZ - lowZ, 1.0)
    spanY = max(highY - lowY, 1.0)

    # One plan per level, in a row, lowest on the left. A stack drawn as a
    # single plan is every balcony on top of every other, which is exactly the
    # picture that hides whether a hole is in the right place.
    levels = sorted({round(s["height"], 3) for s in model["surfaces"]})

    planW, planH = spanX * scale, spanZ * scale
    elevH = spanY * scale
    gap = margin

    width = int(max(len(levels) * planW + (len(levels) + 1) * gap, (spanX * .92 + spanZ * .42) * scale + gap * 2))
    isoH = (spanY * .62 + spanZ * .30) * scale + spanY * scale * .62 + 60
    height = int(planH + elevH + isoH + margin * 5)

    image = Image.new("RGB", (width, height), PAPER)
    pen = ImageDraw.Draw(image, "RGBA")

    def planAt(column, p):
        return (gap + column * (planW + gap) + (p[0] - lowX) * scale, margin + (p[2] - lowZ) * scale)

    def elev(p):
        return (gap + (p[0] - lowX) * scale, margin * 3 + planH + (highY - p[1]) * scale)

    # ---- a plan per level -------------------------------------------------
    for column, level in enumerate(levels):
        pen.text((gap + column * (planW + gap), margin - 14), "level %.2f m" % level, fill=INK)

        # The level below, ghosted, so a ramp coming up can be seen arriving.
        for surface in model["surfaces"]:
            if surface["height"] < level - .05:
                pen.polygon([planAt(column, p) for p in surface["outer"]], fill=(0, 0, 0, 14))

        for surface in model["surfaces"]:
            if abs(surface["height"] - level) > .05:
                continue

            colour = KINDS.get(surface["kind"], STOREY)
            pen.polygon([planAt(column, p) for p in surface["outer"]], fill=colour + (230,), outline=INK)

            for hole in surface["holes"]:
                pen.polygon([planAt(column, p) for p in hole], fill=PAPER, outline=INK)

        # Every ramp that touches this level, at either end.
        for ramp in model["ramps"]:
            quad = ramp["quad"]
            footY, headY = quad[0][1], quad[2][1]

            if abs(footY - level) > .05 and abs(headY - level) > .05:
                continue

            rising = abs(footY - level) < .05
            pen.polygon([planAt(column, p) for p in quad], fill=RAMP + (110 if not rising else 190,), outline=INK)

            foot = ((quad[0][0] + quad[1][0]) * .5, 0, (quad[0][2] + quad[1][2]) * .5)
            head = ((quad[2][0] + quad[3][0]) * .5, 0, (quad[2][2] + quad[3][2]) * .5)
            pen.line([planAt(column, foot), planAt(column, head)], fill=PAPER, width=2)

        for barrier in model.get("barriers", []):
            if abs(barrier["base"] - level) > .05 and not (barrier["base"] < level < barrier["base"] + barrier["height"]):
                continue

            a = (barrier["a"][0], 0, barrier["a"][1])
            b = (barrier["b"][0], 0, barrier["b"][1])
            pen.line([planAt(column, a), planAt(column, b)], fill=BARRIER, width=4 if barrier["wall"] else 3)

        for portal in model["portals"]:
            a = portal["at"]

            if abs(a[1] - level) > .05:
                continue

            out = portal["out"]
            stand = (a[0] + out[0] * portal["into"], a[1], a[2] + out[1] * portal["into"])
            pen.line([planAt(column, a), planAt(column, stand)], fill=DOOR, width=2)
            x, y = planAt(column, stand)
            pen.ellipse([x - 4, y - 4, x + 4, y + 4], fill=DOOR)

        for fault in model.get("faults", []):
            if abs(fault["where"][1] - level) > .05:
                continue

            x, y = planAt(column, fault["where"])
            pen.line([(x - 6, y - 6), (x + 6, y + 6)], fill=FAULT, width=3)
            pen.line([(x - 6, y + 6), (x + 6, y - 6)], fill=FAULT, width=3)

    # ---- the elevation ----------------------------------------------------
    pen.text((gap, margin * 3 + planH - 14), "elevation along the wall", fill=INK)

    for surface in sorted(model["surfaces"], key=lambda s: s["height"]):
        lo = min(p[0] for p in surface["outer"])
        hi = max(p[0] for p in surface["outer"])
        colour = KINDS.get(surface["kind"], STOREY)
        top = elev((lo, surface["height"], 0))
        bottom = elev((hi, surface["height"] - .18, 0))
        pen.rectangle([top[0], top[1], bottom[0], bottom[1]], fill=colour, outline=INK)

    for ramp in model["ramps"]:
        quad = ramp["quad"]
        foot = ((quad[0][0] + quad[1][0]) * .5, quad[0][1])
        head = ((quad[2][0] + quad[3][0]) * .5, quad[2][1])
        pen.line([elev((foot[0], foot[1], 0)), elev((head[0], head[1], 0))], fill=RAMP, width=6)

    # Doors last and outlined, so a ramp passing behind one does not read as
    # going through it. An elevation flattens the depth away: a door half a
    # metre off the wall and a ramp a metre off it share a column of pixels
    # and always will, so the drawing has to say which is in front.
    for portal in model["portals"]:
        x, y = elev(portal["at"])
        pen.rectangle([x - 5, y - 17, x + 5, y + 1], fill=PAPER)
        pen.rectangle([x - 4, y - 16, x + 4, y], fill=DOOR, outline=INK)

    # ---- the triangles ----------------------------------------------------
    #
    # The same model again, drawn from the faces the kernel emits rather than
    # from its polygons. Two pictures of one thing: if the triangulation ever
    # stops agreeing with the outlines - a stairwell quietly filled in, a
    # balcony fanned over ground that is not in it - the two stop matching and
    # you can see it without measuring anything.
    faces = model.get("faces", [])

    if faces:
        pen.text((gap, margin * 3 + planH + elevH + 12), "the same model, as triangles", fill=INK)

        shade = {"surface": BALCONY, "ramp": RAMP, "barrier": BARRIER, "solid": SOLID}

        def iso(p):
            # A plain isometric, so the ramps read as sloping.
            # The height term pushes upward, so the whole view is dropped by the
            # tallest thing in the model - otherwise the top of a four storey
            # stack climbs back into the elevation above it.
            return (gap + (p[0] - lowX) * scale * .92 + (p[2] - lowZ) * scale * .42,
                    margin * 4 + planH + elevH + 26 + spanY * scale * .62 + (p[2] - lowZ) * scale * .30 - (p[1] - lowY) * scale * .62)

        # The building first and barely there. It is the biggest thing in the
        # model and the least interesting: drawn solid it hides every surface
        # and ramp behind it, which is the opposite of what this view is for.
        for face in faces:
            if face["kind"] == "solid":
                pen.polygon([iso(p) for p in face["tri"]], fill=SOLID + (40,))

        walking = [f for f in faces if f["kind"] != "solid"]
        walking.sort(key=lambda f: (sum(p[2] for p in f["tri"]) / 3.0, sum(p[1] for p in f["tri"]) / 3.0))

        for face in walking:
            colour = shade.get(face["kind"], STOREY)
            pen.polygon([iso(p) for p in face["tri"]], fill=colour + (235,), outline=INK)

    faults = model.get("faults", [])
    pen.text((gap, height - 16), "no faults" if not faults else "%d faults: %s" % (len(faults), ", ".join(sorted({f["kind"] for f in faults}))), fill=INK if not faults else FAULT)

    image.save(path)
    print("%d surfaces, %d ramps, %d doors, %d faults -> %s" % (len(model["surfaces"]), len(model["ramps"]), len(model["portals"]), len(faults), path))


def main():
    parse = argparse.ArgumentParser()
    parse.add_argument("model")
    parse.add_argument("out")
    parse.add_argument("--scale", type=float, default=42)
    parse.add_argument("--margin", type=int, default=28)
    args = parse.parse_args()

    with open(args.model) as handle:
        draw(json.load(handle), args.out, args.scale, args.margin)


if __name__ == "__main__":
    main()
