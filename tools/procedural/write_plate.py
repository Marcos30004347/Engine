#!/usr/bin/env python3
"""Emit a plate transcription as JSON, from a compact Python literal.

The schema is the plan's (.plan/bayesian-architectural-program.md §3). This
exists so a transcription is written once, in the shape it was read off the
plate, rather than typed out twice as JSON.

A room is (use, qualifier, raw, feet) with anything after `use` optional:
    ("kitchen",)                        no dimension printed
    ("kitchen", "", "KITCHEN", [18, 13])
An adjacency is (a, b, link), where a and b are "use" or "use:qualifier".
"""

import json
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parents[2] / "assets" / "procedural" / "programmes"


def room(entry):
    use, qualifier, raw, feet = (list(entry) + ["", "", None])[:4]
    out = {"use": use}

    if qualifier:
        out["qualifier"] = qualifier

    if raw:
        out["raw"] = raw

    if feet:
        out["feet"] = feet

    return out


def massing(attachment="", plan="", roof="", frontage=0., depth=0.,
            plot_frontage=0., storeys_in_roof=0, features=(), confidence="medium"):
    """The building as a solid, rather than as a list of rooms.

    The programme drives the exterior as well as the interior - Footprint is a
    variable of the network, and the massing takes its outline from there
    rather than from a coverage fraction of the lot - so the corpus has to
    carry what the elevation shows and not only what the plan does.

    `features` are (kind, count, feet) triples with count and feet optional:
    a porch, a bay window, a tower, a conservatory, a veranda, dormers.
    """
    out = {"confidence": confidence}

    for key, value in (("attachment", attachment), ("plan", plan), ("roof", roof)):
        if value:
            out[key] = value

    if frontage:
        out["frontageFeet"] = frontage

    if depth:
        out["depthFeet"] = depth

    if plot_frontage:
        # The allotment, not the building. These designs are sold by frontage -
        # "a frontage of twenty-five feet, leaving a gateway entrance of nine
        # feet" - and the difference between the two numbers is the gateway.
        out["plotFrontageFeet"] = plot_frontage

    if storeys_in_roof:
        out["storeysInRoof"] = storeys_in_roof

    if features:
        entries = []

        for feature in features:
            kind, count, feet = (list(feature) + [0, None])[:3]
            entry = {"kind": kind}

            if count:
                entry["count"] = count

            if feet:
                entry["feet"] = feet

            entries.append(entry)

        out["features"] = entries

    return out


def write(page, kind, storeys, basement, drawn, floors, confidence,
          notes="", source="", same_house_as=None, suffix="", mass=None,
          dwelling=True, adjacency_source="inferred"):
    # A specification page often describes two or three designs at once - the
    # larger and the smaller cottage of one plate, say - so a page can carry
    # more than one programme and the suffix separates them.
    doc = {
        "page": page,
        "source": source or f"The Builder's Practical Director, PDF page {page}",
        "kind": kind,
        "storeys": storeys,
        "basement": basement,
        "floorsDrawn": drawn,
    }

    if not dwelling:
        # A warehouse is not a house. The corpus carries a few buildings that
        # are not dwellings because a town has them in it, and nothing that
        # reasons about houses should count them.
        doc["dwelling"] = False

    if notes:
        doc["notes"] = notes

    if mass:
        doc["massing"] = mass

    if same_house_as is not None:
        # An inference, never a reading: two plates that look like two floors
        # of one house. Kept separate so the record stays what was printed,
        # and flagged so a loader can join them on purpose.
        doc["sameHouseAs"] = same_house_as

    doc["floors"] = [
        {
            "level": level,
            "name": nm,
            "complete": True,
            "rooms": [room(r) for r in rooms],
            "adjacencies": [{"a": a, "b": b, "link": link} for a, b, link in links],
        }
        for level, nm, rooms, links in floors
    ]

    # A stair reaches the landing above it. The specifications state it - "the
    # stairs lead to the bedroom floor, which contains" - and leaving it to be
    # written by hand each time means forgetting it, which strands a whole
    # upper floor and reads in the report as a transcription with no doors.
    stairs = [(r["use"] + (":" + r.get("qualifier", "") if r.get("qualifier") else ""),
               f["level"])
              for f in doc["floors"] for r in f["rooms"] if r["use"] == "staircase"]
    landings = [(r["use"] + (":" + r.get("qualifier", "") if r.get("qualifier") else ""),
                 f["level"])
                for f in doc["floors"] for r in f["rooms"] if r["use"] == "landing"]

    if stairs and landings:
        joined = {(a["a"], a["b"]) for f in doc["floors"] for a in f["adjacencies"]}

        # A big house has two stairs, a principal one and a back one for the
        # servants, and it is the principal one that arrives at the landings.
        # The unqualified "staircase" is the principal one by convention; take
        # the first only when there is nothing else to go on.
        principal = [k for k, _ in stairs if k == "staircase"]
        stair = principal[0] if principal else stairs[0][0]

        for landing, level in landings:
            if (stair, landing) in joined or (landing, stair) in joined:
                continue

            for f in doc["floors"]:
                if f["level"] == level:
                    f["adjacencies"].append({"a": stair, "b": landing, "link": "open"})

                    break

    doc["adjacencySource"] = adjacency_source
    doc["confidence"] = dict(zip(("rooms", "dimensions", "adjacency"), confidence))

    path = OUT / f"plate-{page:03d}{suffix}.json"
    path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {path.name}: {sum(len(f['rooms']) for f in doc['floors'])} rooms, "
          f"{sum(len(f['adjacencies']) for f in doc['floors'])} adjacencies")


if __name__ == "__main__":
    sys.exit("import this and call write(); see the batch scripts")
