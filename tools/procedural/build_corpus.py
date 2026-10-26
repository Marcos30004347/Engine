#!/usr/bin/env python3
"""Aggregate every transcribed source into one corpus.json.

The per-design files under assets/procedural/programmes are the record of what
each plate says, and they are what a person edits. This is the other end: one
file carrying all of it in the shape a fitter wants, so that training does not
mean walking a directory and re-deriving the same summaries every time.

It carries three things a per-design file cannot:

  the **vocabulary** actually used, with how many designs carry each room, so
  that pruning a rare room type is a lookup rather than a recount;

  the **co-occurrence and adjacency counts** over the whole corpus, which are
  the sufficient statistics for the pairwise part of the network;

  the **second corpus** - Pantin's medieval town houses - kept beside the
  Victorian one rather than merged into it, because they are different
  building cultures and the paper's own advice is that one network cannot span
  that much structural variation.
"""

import json
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2] / "assets" / "procedural" / "programmes"


def main():
    designs = []

    for path in sorted(ROOT.glob("plate-*.json")):
        designs.append(json.loads(path.read_text()))

    rooms = Counter()
    pairs = Counter()
    links = Counter()
    kinds = Counter()
    attachments = Counter()
    plans = Counter()
    roofs = Counter()
    sources = Counter()
    dimensioned = Counter()
    areas = {}

    for design in designs:
        sources[design.get("adjacencySource", "inferred")] += 1
        kinds[design.get("kind", "")] += 1
        mass = design.get("massing", {})
        attachments[mass.get("attachment", "")] += 1
        plans[mass.get("plan", "")] += 1
        roofs[mass.get("roof", "")] += 1

        here = set()
        index = {}
        flat = []

        for floor in design["floors"]:
            for room in floor["rooms"]:
                key = room["use"] + (":" + room["qualifier"] if room.get("qualifier") else "")
                index.setdefault(key, len(flat))
                index.setdefault(room["use"], len(flat))
                flat.append(room)
                here.add(room["use"])

                if room.get("feet"):
                    a, b = room["feet"]
                    dimensioned[room["use"]] += 1
                    areas.setdefault(room["use"], []).append(round(a * b, 1))

        for use in here:
            rooms[use] += 1

        for a in sorted(here):
            for b in sorted(here):
                if a < b:
                    pairs[a + " + " + b] += 1

        for floor in design["floors"]:
            for link in floor["adjacencies"]:
                ia, ib = index.get(link["a"]), index.get(link["b"])

                if ia is None or ib is None:
                    continue

                x, y = sorted((flat[ia]["use"], flat[ib]["use"]))
                links[x + " + " + y + " (" + link.get("link", "door") + ")"] += 1

    medieval = ROOT / "medieval" / "pantin-catalogue.json"

    corpus = {
        "note": "Training data for the architectural programme network. "
                "See assets/procedural/programmes/README.md.",
        "victorian": {
            "source": "The Builder's Practical Director, 784 page scans",
            "designs": len(designs),
            "dwellings": sum(1 for d in designs if d.get("dwelling", True)),
            "rooms": sum(len(f["rooms"]) for d in designs for f in d["floors"]),
            "adjacencies": sum(len(f["adjacencies"]) for d in designs for f in d["floors"]),
            "adjacencySources": dict(sources),
            "buildingKinds": dict(kinds.most_common()),
            "attachment": {k: v for k, v in attachments.most_common() if k},
            "planShape": {k: v for k, v in plans.most_common() if k},
            "roof": {k: v for k, v in roofs.most_common() if k},
            "roomFrequency": dict(rooms.most_common()),
            "roomsDimensioned": dict(dimensioned.most_common()),
            "roomAreasSqFt": {k: sorted(v) for k, v in sorted(areas.items())},
            "coOccurrence": dict(pairs.most_common(60)),
            "adjacencyCounts": dict(links.most_common(80)),
            "designFiles": [d.get("page") for d in designs],
        },
    }

    if medieval.exists():
        pantin = json.loads(medieval.read_text())
        corpus["medieval"] = {
            "source": pantin["source"],
            "houses": len(pantin["houses"]),
            "planTypes": pantin["types"],
            "planTypeCounts": dict(Counter(h["planType"] for h in pantin["houses"])),
            "roomFrequency": dict(Counter(
                r for h in pantin["houses"] for r in h["roomsMentioned"]).most_common()),
            "dimensionedRooms": sum(len(h["dimensioned"]) for h in pantin["houses"]),
        }

    out = ROOT / "corpus.json"
    out.write_text(json.dumps(corpus, indent=1) + "\n", encoding="utf-8")

    print(f"wrote {out.relative_to(Path.cwd())}")
    print(f"  victorian: {corpus['victorian']['designs']} designs, "
          f"{corpus['victorian']['rooms']} rooms, "
          f"{corpus['victorian']['adjacencies']} adjacencies")

    if "medieval" in corpus:
        print(f"  medieval:  {corpus['medieval']['houses']} houses, "
              f"{corpus['medieval']['dimensionedRooms']} dimensioned rooms")


if __name__ == "__main__":
    main()
