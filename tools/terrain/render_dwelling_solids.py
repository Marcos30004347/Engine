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


def view(yaw_degrees, pitch_degrees):
    """Point the camera somewhere else.

    Half a turn matters more than it sounds. The buildings are planned with
    their doors on one side, so a fixed view shows every one of them from the
    back - and the entrance bay, the shop front and the posts are all on the
    side you cannot see. The default is kept because most of these buildings
    are judged on their massing, which reads from anywhere.
    """
    global YAW, PITCH

    YAW = math.radians(yaw_degrees)
    PITCH = math.radians(pitch_degrees)

LIGHT = (-0.45, 0.78, -0.44)

PALETTE = {
    "garden": (150, 168, 118),
    "yard": (201, 193, 168),
    "walls": (214, 203, 182),
    "innerwalls": (196, 186, 166),
    "pillars": (92, 78, 62),
    "slopes": (150, 82, 62),
    "soffits": (110, 62, 48),
    "gables": (196, 184, 162),

    # The two things the building does not have, drawn rather than omitted.
    #
    # A wall that is missing and a wall that was never emitted look identical
    # in a render, and only one of them is a building - so an entrance bay's
    # open front and the holes cut for doors and windows are painted, in a
    # colour nothing else uses, over whatever stands behind them.
    "openwalls": (78, 168, 190),

    # A door and a window are the same hole to the geometry and not to the eye,
    # so they are not the same colour. Nothing is modelled in either: the plane
    # is a marker saying "joinery goes here", which is exactly what the
    # generator commits to and no more.
    # The faces the hole exposes as it passes through the wall: the reveal.
    # Solid, and darker than the wall, because this is the one surface that
    # says an opening has depth.
    "reveals": (150, 141, 126),
    "innerreveals": (150, 141, 126),

    # The top of a wall, seen from above.
    "caps": (188, 178, 158),

    # The frame. Two colours for two sections, because the ratio between them
    # is the structure: drawn alike, a floor is a grid of identical sticks.
    "heavytimbers": (108, 74, 44),
    "timbers": (150, 112, 72),

    # The joinery set into an opening.
    "joinery": (206, 196, 176),

    # The frame of the outside wall: the one timber a street sees.
    "framing": (94, 68, 46),

    # Masonry, and the only masonry on a timber building: the stack.
    "chimneys": (150, 128, 114),
    "innerchimneys": (138, 116, 102),
    "ceilings": (196, 186, 170),

    # The boards over the joists.
    "decking": (163, 139, 106),

    # ...and the flights between them.
    "stairs": (139, 114, 82),
    "furniture": (116, 82, 54),
    "dormerwalls": (166, 142, 108),
    "dormerwindows": (88, 138, 176),
    "dormerroofs": (142, 62, 45),
    "dormerceilings": (196, 186, 170),
    "foundations": (68, 72, 76),
    "s_foundation": (68, 72, 76),

    # Exterior circulation is deliberately high-contrast: these renders are
    # proofs of openings, guarded balcony landings and stair continuity.
    "apartmentwalls": (207, 194, 170),
    "balconies": (224, 177, 91),
    "exteriorstairs": (74, 142, 188),
    "balconyrails": (61, 72, 83),
    "apartmentdoors": (44, 102, 72),

    # Fortress junction anatomy.  In particular, joiners must never disappear
    # into the wall colour: they are the structural answer to an oblique arm.
    "fortresstower": (63, 139, 132),
    "fortresswalls": (54, 90, 190),
    "fortressjoiners": (224, 55, 55),
    "fortressdeck": (225, 174, 74),
    "fortressbelfry": (117, 80, 54),
    "fortressroof": (139, 59, 47),
    "fortressstairs": (83, 157, 92),
    "fortressturns": (238, 45, 190),
    "fortressturnclosures": (231, 47, 36),
    "fortressopeningrail": (42, 190, 210),

    # The skeleton view: the structure's own faces, one colour per kind. No
    # cuts, no reveals - the point is to see what the mesh was built from.
    "s_wall": (184, 176, 160),
    "s_windowed_wall": (96, 140, 190),
    "s_door_wall": (200, 120, 60),
    "s_double_door_wall": (220, 90, 40),
    "s_arch_wall": (230, 170, 70),
    "s_parapet": (150, 140, 120),
    "s_fence": (130, 100, 60),
    "s_rail": (110, 80, 50),
    "s_roof": (156, 74, 54),
    "s_gable": (120, 60, 45),
    "s_soffit": (100, 50, 40),
    "s_deck": (170, 150, 120),
    "s_cap": (140, 60, 50),
    "s_pier": (150, 150, 140),
    "s_post": (120, 100, 70),
    "s_floor": (190, 170, 130),
    "s_paving": (210, 200, 170),
    "s_ramp": (90, 150, 110),
    "s_landing": (70, 120, 90),
    "s_timber": (100, 80, 40),
    "s_furniture": (142, 96, 58),
    "s_dormer_opening": (92, 82, 72),
    "s_dormer_wall": (178, 142, 94),
    "s_dormer_window": (72, 148, 202),
    "s_dormer_roof": (176, 72, 52),
    "s_dormer_ceiling": (204, 190, 156),
}

# How much of what is behind a surface shows through it. Absent means opaque.
#
# The open front of a bay is a plane with nothing on it, so drawing it solid
# hides the posts and the inside of the porch - which is the one thing the
# reader is looking for. Drawn at a third it reads as a marked opening with the
# building visible through it.
ALPHA = {"openwalls": 0.34, "s_dormer_opening": 0.22, "apartmentdoors": 0.32}

# Which faces are stroked.
#
# Nothing.
#
# A wall is not one face: it is split into panels around every opening, and each
# panel outlined draws a seam where there is no edge - so a blank wall came out
# ruled with hairlines that belong to the tessellation rather than to the
# building. Shading already separates every surface from its neighbours, which
# is all an outline was doing, and without them the render is of the building
# instead of the mesh.
OUTLINED = ()

# Skeleton wall kinds come in four tints, `s_wall_0` .. `s_wall_3`, so that
# neighbouring SEGMENTS of one kind can be told apart: a wall is one segment
# per room side, per storey level, per metre slice, and one flat colour hid
# all of that.
SEGMENT_TINTS = (0.72, 1.0, 0.86, 1.14)

for _base in ("s_wall", "s_windowed_wall", "s_door_wall", "s_double_door_wall", "s_arch_wall", "s_parapet", "s_fence", "s_rail"):
    for _v, _k in enumerate(SEGMENT_TINTS):
        PALETTE[f"{_base}_{_v}"] = tuple(min(255, int(c * _k)) for c in PALETTE[_base])

# Ground first, so the flat pieces do not steal the depth test from the things
# standing on them when the two touch.
KINDS = ("garden", "yard", "walls", "innerwalls", "pillars", "slopes", "soffits", "gables", "caps", "decking", "stairs", "furniture", "dormerwalls", "dormerwindows", "dormerroofs", "dormerceilings", "chimneys", "innerchimneys", "ceilings", "reveals", "innerreveals", "heavytimbers", "timbers", "framing", "joinery", "openwalls",
         "foundations", "s_foundation", "apartmentwalls", "balconies", "exteriorstairs", "balconyrails", "apartmentdoors",
         "fortresstower", "fortresswalls", "fortressjoiners", "fortressdeck", "fortressbelfry", "fortressroof", "fortressstairs", "fortressturns", "fortressturnclosures", "fortressopeningrail",
         "s_paving", "s_floor", "s_wall", "s_windowed_wall", "s_door_wall", "s_double_door_wall", "s_arch_wall", "s_parapet", "s_fence", "s_rail", "s_pier", "s_post", "s_ramp", "s_landing", "s_timber", "s_furniture", "s_dormer_opening", "s_dormer_wall", "s_dormer_window", "s_dormer_roof", "s_dormer_ceiling", "s_roof", "s_gable", "s_soffit", "s_deck", "s_cap") + tuple(
    f"{base}_{v}" for base in ("s_wall", "s_windowed_wall", "s_door_wall", "s_double_door_wall", "s_arch_wall", "s_parapet", "s_fence", "s_rail") for v in range(4))

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


def storey(building, level):
    """One storey of a building: the faces standing between its base and top.

    Sliced by height rather than by any structure in the document, because a
    face does not know which floor it belongs to and nothing downstream should
    have to care. The roof is dropped - it is not on any storey - and a face is
    kept when its middle is inside the band, which is exact for a wall and near
    enough for everything else.
    """
    bands = building.get("storeys", [])

    if level >= len(bands):
        return None

    base, top = bands[level]
    kept = {"name": building["name"], "storeys": bands}

    for group in ("exterior", "interior"):
        if group not in building:
            continue

        kept[group] = {}

        for kind, faces in building[group].items():
            # The roof is not on any storey, and neither is the ceiling as far
            # as looking at one goes: a storey seen from above with its own
            # ceiling drawn is a grey lid with the rooms underneath it. The
            # ceiling belongs to the storey and is dropped from the view of it
            # for the same reason the roof is dropped from a plan.
            if kind in ROOF or kind in ("garden", "yard", "ceilings"):
                continue

            inside = []

            for face in faces:
                if not face:
                    continue

                middle = sum(v[1] for v in face) / len(face)

                # Open at the top, closed at the bottom. The boards at a
                # storey's ceiling are the *floor of the one above*, and
                # counted into this one they lie over it like a lid and hide
                # everything the view was opened to show.
                if base - 0.05 <= middle < top - 0.02:
                    inside.append(face)

            kept[group][kind] = inside

    return kept


def parts(building, want):
    """The face lists of a building, by group.

    A building arrives as two meshes - `exterior` and `interior` - because that
    is how they are meant to be loaded: a street needs every building's outside
    and none of their insides. Older documents are one flat object, and are read
    as an exterior with nothing behind it.
    """
    if "exterior" not in building and "interior" not in building:
        return [building] if want in ("both", "exterior") else []

    groups = []

    if want in ("both", "exterior"):
        groups.append(building.get("exterior", {}))

    if want in ("both", "interior"):
        groups.append(building.get("interior", {}))

    return groups


def gather(buildings, want="both"):
    """Every face of every building, projected, with its shade.

    Nothing is culled. The depth buffer decides what is seen, and it is the
    only thing here that can: a hidden face is overwritten by whatever stands
    in front of it.
    """
    out = []

    for group in [g for building in buildings for g in parts(building, want)]:
        for kind in KINDS:
            for face in group.get(kind, []):
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
                    out.append(([project(v) for v in face], rgb, kind in OUTLINED, ALPHA.get(kind, 1.0)))

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

    xs = [p[0] for f, _, _, _ in faces for p in f]
    ys = [p[1] for f, _, _, _ in faces for p in f]
    scale = min((width - 2 * pad) / (max(xs) - min(xs) or 1), (height - 2 * pad) / (max(ys) - min(ys) or 1))
    ox, oy = min(xs), min(ys)

    colour = np.zeros((height, width, 3), dtype=np.float32)
    colour[:] = BACKGROUND
    depth = np.full((height, width), -np.inf, dtype=np.float32)

    # Screen-space triangles, plus the edges to stroke afterwards.
    strokes = []

    for face, rgb, outlined, alpha in faces:
        flat = [(pad + (p[0] - ox) * scale, height - pad - (p[1] - oy) * scale, p[2]) for p in face]

        if outlined:
            strokes.append(flat)

        for k in range(1, len(flat) - 1):
            triangle(colour, depth, flat[0], flat[k], flat[k + 1], rgb, alpha)

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


def triangle(colour, depth, a, b, c, rgb, alpha=1.0):
    """One z-buffered triangle, over its own bounding box only.

    A translucent triangle blends and does not write depth. It is the right
    rule here rather than a compromise: what these mark is an absence - the
    open front of a bay, the hole cut for a door - and an absence that hid what
    is behind it would be worse than not drawing it at all. Because they write
    no depth they also never occlude each other, so the near post of a bay
    shows through its own opening, which is what you want to see.
    """
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

    patch = colour[miny : maxy + 1, minx : maxx + 1]

    if alpha >= 1.0:
        window[nearer] = z[nearer]
        patch[nearer] = rgb

        return

    patch[nearer] = patch[nearer] * (1.0 - alpha) + np.asarray(rgb, dtype=np.float32) * alpha


# The four views a drawing of a part is made of, and what each one is for.
#
# An engineering drawing is three square-on views and, usually, one of the thing
# in the round - because square-on is where a dimension can be read and in the
# round is where you see what it is. A window in a wall is exactly the case
# these views were invented for: from the front it is a rectangle, from the side
# it is a slot through the thickness, from above it is a gap in the band of the
# wall, and in none of the three can you mistake it for a rectangle painted on.
#
# `project` already draws both, with no special case: its pitch is measured from
# plan, so nought is a plan and a right angle is an elevation, and the yaw turns
# the building under it. The only reason these were not available is that the
# view was a constant.
# The roof is the whole of what a building looks like from above, and none of
# what a *plan* looks like from above. So the top view drops it: what is left is
# the band of the walls, with a gap in it wherever an opening goes through, and
# that is the view an opening is actually checked in.
ROOF = ("slopes", "soffits", "gables")

VIEWS = (("TOP", 0, 0.01, ROOF), ("FRONT", 180, 90, ()), ("SIDE", 270, 90, ()), ("IN THE ROUND", 215, 55, ()))


def orthographic(buildings, size, want="both"):
    """One row of four views per building: top, front, side, and in the round.

    FRONT looks at the side the doors are on. The planner puts the entrance on
    the z = 0 edge, and depth in `project` grows towards the camera with z, so
    the default view is of the back of every building and a half turn is what
    puts the front in front.
    """
    tiles = []

    for building in buildings:
        for name, yaw, pitch, hidden in VIEWS:
            view(yaw, pitch)
            groups = [{k: ([] if k in hidden else v) for k, v in g.items()} for g in parts(building, want)]
            tiles.append((f"{building['name']} - {name}", rasterise(gather([{"exterior": g} for g in groups], "exterior"), size, size, 24)))

    return tiles


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("solids")
    parser.add_argument("out")
    parser.add_argument("--size", type=int, default=420)
    parser.add_argument("--scene", action="store_true", help="draw every building in one view")
    parser.add_argument("--width", type=int, default=1800)
    parser.add_argument("--height", type=int, default=1200)
    parser.add_argument("--yaw", type=float, default=35, help="degrees; 215 looks at the side the doors are on")
    parser.add_argument("--pitch", type=float, default=50, help="degrees from plan; low is nearly overhead")
    parser.add_argument("--part", choices=("both", "exterior", "interior"), default="both", help="which of the two meshes to draw")
    parser.add_argument("--floor", type=int, default=None, help="draw only what stands on this storey, counting from nought")
    parser.add_argument("--orthographic", action="store_true", help="four square-on views per building: top, front, side, and the same building drawn in the round")
    parser.add_argument("--name-contains", help="render only buildings whose names contain this text")
    a = parser.parse_args()

    view(a.yaw, a.pitch)

    document = json.load(open(a.solids))
    buildings = document["buildings"]

    if a.name_contains:
        buildings = [b for b in buildings if a.name_contains.lower() in b.get("name", "").lower()]

        if not buildings:
            raise SystemExit(f"no building name contains {a.name_contains!r}")

    if a.floor is not None:
        buildings = [storey(b, a.floor) for b in buildings]
        buildings = [b for b in buildings if b is not None]

        if not buildings:
            raise SystemExit(f"no building has a storey {a.floor}")

    if a.scene:
        # A town has to be drawn as a town: buildings one to a tile cannot show
        # whether they crowd a street or stand off it.
        image = rasterise(gather(buildings, a.part), a.width, a.height, 24)

        if image is None:
            raise SystemExit("no buildings with faces")

        image.save(a.out)
        print(f"{len(buildings)} buildings -> {a.out} ({image.width}x{image.height})")

        return

    if a.orthographic:
        tiles = orthographic(buildings, a.size, a.part)
    else:
        tiles = [(b["name"], rasterise(gather([b], a.part), a.size, a.size, 26)) for b in buildings]

    tiles = [(name, image) for name, image in tiles if image is not None]

    if not tiles:
        raise SystemExit("no buildings with faces")

    columns = 4 if a.orthographic else min(4, len(tiles))
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
