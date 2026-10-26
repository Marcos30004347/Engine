#!/usr/bin/env python3
"""Draw the per-class programme gallery as floor plans, one row per building.

`ProgrammeClassesTests` writes `classes-gallery.json`: every building class laid
out by Merrell's optimiser, with each room's outline, use and floor. Nothing
drew it, so the only way to look at a peasant cottage was to read a line of
statistics or infer it from a render of its walls - which is how a loft on the
ground floor and a well counted as a room went unnoticed.

A plan is what shows those. Rooms are drawn by purpose with their names and
areas, one panel per floor, at a scale written under each row.
"""

import argparse
import json

from PIL import Image, ImageDraw, ImageFont

# Rooms by what they are for, so a plan can be read at a glance rather than by
# looking up every label. Circulation is pale because it is the space between
# the rooms, service is cool, living is warm, and anything to do with animals
# or stores is earth.
COLOURS = {
    "living room": (214, 176, 134), "parlour": (214, 176, 134),
    "hall": (208, 170, 128), "dining room": (206, 168, 126),
    "bedroom": (186, 166, 190), "dressing room": (196, 178, 198),
    "kitchen": (166, 190, 178), "scullery": (158, 184, 172),
    "pantry": (150, 176, 164), "larder": (150, 176, 164),
    "store": (172, 166, 150), "cellar": (150, 146, 134),
    "byre": (170, 150, 122), "barn": (164, 144, 116),
    "stable": (170, 150, 122), "loft": (182, 168, 146),
    "staircase": (232, 226, 214), "landing": (236, 230, 220),
    "screens passage": (236, 230, 220), "passage": (236, 230, 220),
    "lobby": (238, 232, 222), "gallery": (232, 226, 214),
    "shop": (206, 186, 150), "workshop": (190, 174, 146),
    "office": (196, 186, 170), "taproom": (210, 180, 140),
}

OUTSIDE = {"yard", "courtyard", "croft", "garden", "orchard", "pen", "midden",
           "well", "exercise yard"}


def colour(use):
    return COLOURS.get(use, (198, 192, 182))


def draw(panel, rooms, box, scale, origin, font, small):
    """One floor, with every room filled, outlined and named."""
    for room in rooms:
        ring = room.get("outline") or []

        if len(ring) < 3:
            continue

        points = [(origin[0] + (x - box[0]) * scale, origin[1] + (box[3] - z) * scale)
                  for x, z in ring]
        use = room.get("use", "")
        fill = colour(use)

        panel.polygon(points, fill=fill, outline=(70, 66, 60))

        # The name, and the area it actually got - the pair is the whole story
        # when a room asked for one thing and the optimiser gave it another.
        cx = sum(p[0] for p in points) / len(points)
        cy = sum(p[1] for p in points) / len(points)
        width = max(p[0] for p in points) - min(p[0] for p in points)

        if width < 34:
            continue

        label = use if width > 74 else use.split()[0]
        panel.text((cx, cy - 6), label, fill=(40, 38, 34), font=small, anchor="mm")
        panel.text((cx, cy + 5), "%.0f m2" % room.get("got", 0), fill=(96, 92, 86),
                   font=small, anchor="mm")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("gallery")
    parser.add_argument("out")
    parser.add_argument("--cell", type=int, default=300)
    parser.add_argument("--match", default="", help="only classes whose name contains this")
    args = parser.parse_args()

    document = json.load(open(args.gallery))
    buildings = [b for b in document["buildings"] if args.match in b.get("class", "")]

    if not buildings:
        raise SystemExit("no buildings matched")

    try:
        font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 13)
        small = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 9)
    except OSError:
        font = small = ImageFont.load_default()

    # A row per building, a column per floor, so floors of one building line up
    # and buildings can be compared down the page.
    widest = max(len({r.get("floor", 0) for r in b.get("rooms", [])}) for b in buildings)
    cell, pad, head = args.cell, 10, 22

    image = Image.new("RGB", (pad + widest * (cell + pad), len(buildings) * (cell + head + pad) + pad),
                      (247, 246, 243))
    panel = ImageDraw.Draw(image)

    for row, building in enumerate(buildings):
        rooms = [r for r in building.get("rooms", []) if r.get("use") not in OUTSIDE]

        # The ground the building does not stand on is part of the plan, not
        # scenery: a manor's courtyard, a prison's exercise yard, the croft
        # behind a cottage. It is kept apart from the rooms because it has no
        # floor, and it was going undrawn for the same reason.
        grounds = building.get("grounds", [])
        floors = sorted({r.get("floor", 0) for r in rooms})
        top = pad + row * (cell + head + pad)

        panel.text((pad, top), "%s - %d storeys, %d rooms"
                   % (building.get("class", "?"), building.get("storeys", 0), len(rooms)),
                   fill=(40, 38, 34), font=font)

        # One scale for every floor of a building, or an upper floor drawn on
        # its own would look the size of the ground.
        every = [p for r in rooms + grounds for p in (r.get("outline") or [])]

        if not every:
            continue

        box = (min(p[0] for p in every), min(p[1] for p in every),
               max(p[0] for p in every), max(p[1] for p in every))
        span = max(box[2] - box[0], box[3] - box[1], 1e-6)
        scale = (cell - 16) / span

        for column, floor in enumerate(floors):
            left = pad + column * (cell + pad)
            origin = (left + 8, top + head + 8)

            panel.rectangle([left, top + head, left + cell, top + head + cell],
                            fill=(255, 254, 252), outline=(214, 210, 202))
            # Ground first and underneath, so a room standing on it reads as
            # standing on it.
            if floor == floors[0]:
                for ground in grounds:
                    ring = ground.get("outline") or []

                    if len(ring) < 3:
                        continue

                    panel.polygon([(origin[0] + (x - box[0]) * scale,
                                    origin[1] + (box[3] - z) * scale) for x, z in ring],
                                  fill=(214, 224, 206), outline=(180, 192, 172))
                    cx = sum(p[0] for p in ring) / len(ring)
                    cz = sum(p[1] for p in ring) / len(ring)
                    panel.text((origin[0] + (cx - box[0]) * scale,
                                origin[1] + (box[3] - cz) * scale),
                               ground.get("use", ""), fill=(110, 124, 102), font=small, anchor="mm")

            draw(panel, [r for r in rooms if r.get("floor", 0) == floor], box, scale, origin,
                 font, small)
            panel.text((left + 6, top + head + cell - 13),
                       "floor %d - %.0f mm to the metre" % (floor, scale * 1000 / 1000 * 10),
                       fill=(120, 116, 110), font=small)

    image.save(args.out)
    print("%d buildings -> %s (%dx%d)" % (len(buildings), args.out, image.width, image.height))


if __name__ == "__main__":
    main()
