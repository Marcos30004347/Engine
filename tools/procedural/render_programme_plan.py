#!/usr/bin/env python3
"""Draw the planned buildings as floor plans, one panel per floor.

A number cannot settle an argument about a plan. "77% of doors delivered" says
the geometry mostly works and says nothing about whether the result reads as a
building, and the questions that actually come up are questions about the
picture: is the shop on the street, is the hall in the middle and touching
everything, is that room a splinter, does the plan have a front and a back.

So this draws no shading and no furniture. Each floor is its own panel, rooms
are filled by what they are for - service, polite, private, circulation, trade,
outdoor - and the things that are supposed to be checked are marked rather than
left to the eye:

  a door           a black bar across the wall it sits in
  an open wall     a hollow bar, because a cased opening is not a door
  a street room    a heavy edge on the frontage side
  an area miss     the room's label carries the percentage it came out wrong

    render_programme_plan.py programme-gallery.json out.png
"""

import json
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# Room colour by what the room is for, not by what it is called. Six families,
# because a plan is read by finding the service end and the polite end before
# any individual room is identified.
FAMILY = {
    "service": ("kitchen scullery larder pantry dairy washhouse cellar brewhouse "
                "still room housekeeper's room butler's room servants hall buttery "
                "undercroft store strong room", (214, 199, 168)),
    "polite": ("parlour living room dining room drawing room breakfast room study "
               "library morning room saloon ballroom smoking room gun room "
               "great hall solar council chamber", (206, 214, 196)),
    "private": ("bedroom dressing room nursery boudoir closet bathroom water closet "
                "cell", (200, 198, 214)),
    "circulation": ("hall passage landing staircase lobby porch narthex "
                    "screens passage gallery cloister aisle", (232, 228, 220)),
    "trade": ("shop workshop office counting house bar tap room club room "
              "coffee room billiard room dispensary waiting room consulting room "
              "school room infirmary guardroom gatehouse armoury barracks", (222, 206, 190)),
    "sacred": ("nave chancel sanctuary transept side chapel vestry crypt belfry "
               "chapter house tower keep", (196, 206, 216)),
    "outdoor": ("yard courtyard exercise yard veranda conservatory stable barn loft "
                "dungeon well", (186, 200, 182)),
}

LOOKUP = {}

for family, (words, colour) in FAMILY.items():
    for word in words.split("  "):
        pass

# The word lists above are space separated but room names contain spaces, so
# build the lookup from explicit lists instead.
FAMILY_ROOMS = {
    "service": ["kitchen", "scullery", "larder", "pantry", "dairy", "washhouse",
                "cellar", "brewhouse", "still room", "housekeeper's room",
                "butler's room", "servants hall", "buttery", "undercroft",
                "store", "strong room"],
    "polite": ["parlour", "living room", "dining room", "drawing room",
               "breakfast room", "study", "library", "morning room", "saloon",
               "ballroom", "smoking room", "gun room", "great hall", "solar",
               "council chamber"],
    "private": ["bedroom", "dressing room", "nursery", "boudoir", "closet",
                "bathroom", "water closet", "cell"],
    "circulation": ["hall", "passage", "landing", "staircase", "lobby", "porch",
                    "narthex", "screens passage", "gallery", "cloister", "aisle"],
    "trade": ["shop", "workshop", "office", "counting house", "bar", "tap room",
              "club room", "coffee room", "billiard room", "dispensary",
              "waiting room", "consulting room", "school room", "infirmary",
              "guardroom", "gatehouse", "armoury", "barracks"],
    "sacred": ["nave", "chancel", "sanctuary", "transept", "side chapel", "vestry",
               "crypt", "belfry", "chapter house", "tower", "keep"],
    "outdoor": ["yard", "courtyard", "exercise yard", "veranda", "conservatory",
                "stable", "barn", "loft", "dungeon", "well"],
}

COLOUR = {family: FAMILY[family][1] for family in FAMILY}

for family, rooms in FAMILY_ROOMS.items():
    for room in rooms:
        LOOKUP[room] = COLOUR[family]

INK = (40, 38, 34)
FAINT = (150, 146, 140)
PAPER = (250, 248, 244)


def font(size):
    for name in ("/System/Library/Fonts/Supplemental/Arial.ttf",
                 "/System/Library/Fonts/Helvetica.ttc",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue

    return ImageFont.load_default()


GROUND = {
    "yard": (226, 224, 210),
    "croft": (214, 224, 198),
    "garden": (204, 222, 196),
    "orchard": (198, 218, 190),
    "pen": (226, 218, 198),
    "midden": (208, 200, 186),
    "courtyard": (228, 226, 218),
}


def floors_of(building):
    return sorted({room["floor"] for room in building["rooms"]})


def draw_building(image, draw, building, box, small, tiny):
    """Draw every floor of one building side by side inside `box`."""
    x0, y0, x1, y1 = box
    floors = floors_of(building)

    if not floors:
        return

    # One scale for every floor of a building, so the floors compare.
    # Scaled to the building and its allocated ground, not to the whole lot: a
    # mansion in a park drawn to the park is a stamp in a field, and the plan
    # is the thing being read. The lot is still drawn, and runs off the panel
    # where it is larger.
    points = [p for room in building["rooms"] for p in room["outline"]]
    points += building["outline"]

    if not points:
        points = building.get("lot", [])

    # A margin of a few metres, so the yard and the lot show as a border rather
    # than deciding the scale. A mansion stands in a park; drawn to the park it
    # is a stamp in a field, and the plan is the thing being read.
    margin = 4.0
    lowX = min(p[0] for p in points) - margin
    highX = max(p[0] for p in points) + margin
    lowZ = min(p[1] for p in points) - margin
    highZ = max(p[1] for p in points) + margin

    header = 34
    panelW = (x1 - x0) / len(floors)
    panelH = y1 - y0 - header
    spanX = max(highX - lowX, 1e-6)
    spanZ = max(highZ - lowZ, 1e-6)
    scale = min((panelW - 14) / spanX, (panelH - 14) / spanZ)

    label = f"{building['label']} - {building['class']}"
    draw.text((x0, y0), label, font=small, fill=INK)
    draw.text((x0, y0 + 12),
              building.get("household",
                           f"{building['storeys']} storey")
              + f" - doors {building['doorsDelivered'] * 100:.0f}%",
              font=tiny, fill=FAINT)

    placed = {}

    for index, floor in enumerate(floors):
        px = x0 + index * panelW
        py = y0 + header

        # Each floor is drawn into its own panel and pasted, so the lot and the
        # grounds - which are larger than the building and run off the edge -
        # are clipped by it instead of painting over the next design.
        panel = Image.new("RGB", (max(1, int(panelW)), max(1, int(panelH))), PAPER)
        panelDraw = ImageDraw.Draw(panel)

        def to_pixels(point):
            return (7 + (point[0] - lowX) * scale,
                    7 + (highZ - point[1]) * scale)

        # The floor, and how tall it is: a mansion's ground floor is a foot
        # or two taller than the bedroom floor over it, and the elevation
        # follows from that, so the height belongs on the drawing.
        name = {(-1): "basement"}.get(floor, f"floor {floor}")
        height = next((c["height"] for c in building.get("ceilings", [])
                       if c["floor"] == floor), None)

        if height:
            name += f"  -  {height:.2f} m to the ceiling"

        draw.text((px + 7, py - 12), name, font=tiny, fill=FAINT)

        # The lot the town allocated, under everything: the building has to
        # stand inside it, and a picture is the only way to see that it does.
        lot = building.get("lot", [])

        if len(lot) >= 3:
            ring = [to_pixels(p) for p in lot]
            panelDraw.polygon(ring, fill=(236, 233, 226), outline=(196, 190, 178))

        # The ground around the building, allocated: yard, croft, pen, garden.
        if floor == 0:
            for ground in building.get("grounds", []):
                outline = [to_pixels(p) for p in ground["outline"]]

                if len(outline) < 3:
                    continue

                panelDraw.polygon(outline, fill=GROUND.get(ground["use"], (223, 228, 214)),
                             outline=(186, 190, 176))
                cx = sum(p[0] for p in outline) / len(outline)
                cy = sum(p[1] for p in outline) / len(outline)
                panelDraw.text((cx - len(ground["use"]) * 2.4, cy - 4), ground["use"],
                          font=tiny, fill=(120, 124, 112))

        for i, room in enumerate(building["rooms"]):
            if room["floor"] != floor:
                continue

            outline = [to_pixels(p) for p in room["outline"]]

            if len(outline) < 3:
                continue

            panelDraw.polygon(outline, fill=LOOKUP.get(room["use"], (226, 224, 220)),
                         outline=INK)
            placed[i] = outline

            # A street room gets its frontage edge drawn heavy.
            if room.get("street"):
                panelDraw.line(outline + [outline[0]], fill=(150, 60, 40), width=3)

            cx = sum(p[0] for p in outline) / len(outline)
            cy = sum(p[1] for p in outline) / len(outline)
            area = room["got"]
            name = room["use"]

            if area >= 6:
                panelDraw.text((cx - len(name) * 2.4, cy - 7), name, font=tiny, fill=INK)
                panelDraw.text((cx - 12, cy + 2), f"{area:.0f} m2", font=tiny, fill=FAINT)

        # Doors, drawn where the geometry actually put them.
        for door in building["doors"]:
            if door["a"] not in placed or door["b"] not in placed:
                continue

            if building["rooms"][door["a"]]["floor"] != floor:
                continue

            dx, dy = to_pixels((door["x"], door["z"]))
            radius = 3.5

            if door["door"]:
                panelDraw.ellipse([dx - radius, dy - radius, dx + radius, dy + radius],
                             fill=(40, 38, 34))
            else:
                panelDraw.ellipse([dx - radius, dy - radius, dx + radius, dy + radius],
                             outline=(40, 38, 34), width=2)

        # The way in, drawn heavy: it is the one door that decides how the
        # building meets the street.
        for entrance in building.get("entrances", []):
            if building["rooms"][entrance["room"]]["floor"] != floor:
                continue

            ex, ey = to_pixels((entrance["x"], entrance["z"]))
            panelDraw.ellipse([ex - 6, ey - 6, ex + 6, ey + 6], fill=(168, 62, 40),
                         outline=(60, 24, 16), width=2)

        image.paste(panel, (int(px), int(py)))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)

    gallery = json.loads(Path(sys.argv[1]).read_text())["buildings"]

    small = font(13)
    tiny = font(10)

    # A mansion has four floors and twenty rooms a floor; it needs the width.
    widest = max((len(floors_of(b)) for b in gallery), default=1)
    columns = 1 if widest > 3 else 2
    rows = (len(gallery) + columns - 1) // columns
    cellW, cellH = (1500, 420) if columns == 1 else (760, 320)
    margin = 24

    image = Image.new("RGB", (columns * cellW + margin * 2,
                              rows * cellH + margin * 2 + 40), PAPER)
    draw = ImageDraw.Draw(image)

    draw.text((margin, margin),
              "Architectural programmes, realised. Rooms coloured by what they are for; "
              "a filled dot is a door and a hollow one an open wall; a red edge is a room "
              "that must face the street.",
              font=small, fill=INK)

    for index, building in enumerate(gallery):
        column = index % columns
        row = index // columns
        x0 = margin + column * cellW
        y0 = margin + 40 + row * cellH
        draw_building(image, draw, building, (x0, y0, x0 + cellW - 20, y0 + cellH - 20),
                      small, tiny)

    image.save(sys.argv[2])
    print(f"wrote {sys.argv[2]}: {len(gallery)} buildings")


if __name__ == "__main__":
    main()
