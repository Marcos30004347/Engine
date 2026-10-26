#!/usr/bin/env python3
"""Triage the builder's book down to the plates that carry a house plan.

Stage 1 of .plan/bayesian-architectural-program.md. The book is 784 scanned
pages; the architectural programmes live on the plates, and the plates are
found by the room labels printed on them, not by the OCR prose (which mangles
the feet-and-inches marks, see the plan's section 2).

The filter is the plan's: a page is a candidate when its drawing-label block
names at least two rooms from the vocabulary. Engineering plates - retaining
walls, sections, mouldings - name none, and fall out on their own.

Usage:
  plate_triage.py [--book PATH] [--min-rooms 2] [--json OUT]
"""

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

DEFAULT_BOOK = Path.home() / "Desktop" / "building-book-markdown-and-images (1)" / "building-book.md"

# Every spelling the book uses, mapped to the normalised vocabulary. The raw
# label is kept in the transcription; this table exists only so two spellings
# of one room do not count as two rooms during triage.
#
# Order matters: the longest spelling that matches wins, so "dining room" is
# not eaten by "room" and "water closet" is not eaten by "closet".
VOCABULARY = {
    "breakfast room": "breakfast_room",
    "drawing room": "drawing_room",
    "dining room": "dining_room",
    "living room": "living_room",
    "sitting room": "sitting_room",
    "bed room": "bedroom",
    "bedroom": "bedroom",
    "best room": "parlour",
    "water closet": "wc",
    "w. c.": "wc",
    "w.c.": "wc",
    "wash-house": "washhouse",
    "wash house": "washhouse",
    "washhouse": "washhouse",
    "back kitchen": "scullery",
    "kitchen": "kitchen",
    "scullery": "scullery",
    "larder": "larder",
    "pantry": "pantry",
    "dairy": "dairy",
    "brewhouse": "brewhouse",
    "bakehouse": "bakehouse",
    "cellar": "cellar",
    "basement": "basement",
    "parlour": "parlour",
    "parlor": "parlour",
    "chamber": "chamber",
    "dressing room": "dressing_room",
    "dressing": "dressing_room",
    "nursery": "nursery",
    "study": "study",
    "library": "library",
    "office": "office",
    "shop": "shop",
    "warehouse": "warehouse",
    "workshop": "workshop",
    "store": "store",
    "stable": "stable",
    "coach house": "coach_house",
    "cow house": "cow_house",
    "piggery": "piggery",
    "hall": "hall",
    "lobby": "lobby",
    "porch": "porch",
    "passage": "passage",
    "landing": "landing",
    "staircase": "staircase",
    "stair case": "staircase",
    "stairs": "staircase",
    "closet": "closet",
    "yard": "yard",
    "court": "court",
    "area": "area",
    "bath room": "bathroom",
    "bathroom": "bathroom",
    "conservatory": "conservatory",
    "veranda": "veranda",
    "billiard room": "billiard_room",
    "school room": "school_room",
    "bar": "bar",
    "tap room": "tap_room",
    "club room": "club_room",
}

# Rooms these labels name that are not rooms of a dwelling. Counted, but a
# plate made only of them is not a house plan.
NOT_A_DWELLING_ROOM = {"area", "yard", "court", "basement"}

# Words on a plate that say it is a drawing rather than a specification page.
DRAWING_WORDS = ("plan", "elevation", "section", "scale of feet", "seace of feet",
                 "scale or feet", "ground plan", "chamber plan", "front elevation")

# The building types the prose uses, for conditioning the network later.
BUILDING_KINDS = ("semi-detached", "detached", "labourer", "cottage", "villa",
                  "mansion", "farm house", "farmhouse", "school", "lodge",
                  "tenement", "inn", "public house", "shop", "parsonage",
                  "vicarage", "rectory", "almshouse", "workhouse")

# "12 feet by 10 feet", "14' ,, 6" X 12' ,, 0"", "10 ft. by 7 ft." - the
# specification tables, as the OCR leaves them. The *values* are not reliable
# and must be read off the page image; this only finds the pages that have one.
DIMENSION_RE = re.compile(r"\d\s*(?:feet|ft\.?|['\u2019])\s*[.,\u201e\s]*\d*\s*[\"\u201d]?\s*(?:by|x|X|\u00d7)\s*\d",
                          re.IGNORECASE)

PAGE_RE = re.compile(r"^## PDF page (\d+)\s*$")
LABELS_RE = re.compile(r"^#### Supplemental drawing labels \(OCR\)\s*$")
SECTION_RE = re.compile(r"^#{2,4} ")


def parse_book(path):
    """Split the book into per-page records: labels block, description, prose."""
    pages = {}
    current = None
    mode = None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = PAGE_RE.match(line)
        if m:
            current = {"page": int(m.group(1)), "labels": [], "description": "", "text": []}
            pages[current["page"]] = current
            mode = "head"
            continue
        if current is None:
            continue
        if LABELS_RE.match(line):
            mode = "labels"
            continue
        if SECTION_RE.match(line):
            mode = "text" if line.startswith("### Extracted text") else "head"
            continue
        if mode == "labels":
            if line.strip():
                current["labels"].append(line.strip())
        elif mode == "text":
            current["text"].append(line)
        elif line.startswith("**Image description:**"):
            current["description"] = line[len("**Image description:**"):].strip()
    return pages


def rooms_in(text):
    """Normalised room names named by a blob of label text, longest match first."""
    low = " " + re.sub(r"\s+", " ", text.lower()) + " "
    found = []
    for spelling in sorted(VOCABULARY, key=len, reverse=True):
        pattern = r"(?<![a-z])" + re.escape(spelling).replace(r"\ ", r"[\s\-]+") + r"(?![a-z])"
        if re.search(pattern, low):
            found.append(VOCABULARY[spelling])
            low = re.sub(pattern, " ", low)
    return found


def kind_of(page):
    """The building type, from the plate labels first and the prose second."""
    blob = (" ".join(page["labels"]) + " " + page["description"] + " "
            + " ".join(page["text"][:40])).lower()
    for kind in BUILDING_KINDS:
        if kind in blob:
            return kind.replace("farmhouse", "farm house")
    return ""


def specifications(pages, min_rooms, min_dimensions):
    """Pages whose *prose* is a programme: a room list with dimensions.

    The plan assumed the plates were the only source and the prose was useless,
    because the OCR yields fourteen clean dimensions in the whole book. That is
    true of the OCR and false of the pages: read as images, the specification
    tables are perfectly legible, and they carry what no plate does - the
    door-by-door sequence in words ("the front door opens into a small passage
    ... on the left is the entrance to the living room"). They are the better
    source, and they are easier to transcribe.
    """
    out = []

    for number in sorted(pages):
        page = pages[number]
        prose = "\n".join(page["text"])
        rooms = set(rooms_in(prose))
        dwelling = rooms - NOT_A_DWELLING_ROOM
        dimensions = len(DIMENSION_RE.findall(prose))

        if len(dwelling) < min_rooms or dimensions < min_dimensions:
            continue

        out.append({
            "page": number,
            "rooms": sorted(rooms),
            "dwellingRooms": sorted(dwelling),
            "dimensions": dimensions,
            "kind": kind_of(page),
            "image": f"page-{number:03d}.jpg",
        })

    return out


def triage(pages, min_rooms):
    candidates = []
    for number in sorted(pages):
        page = pages[number]
        labels = " \n ".join(page["labels"])
        rooms = rooms_in(labels)
        dwelling_rooms = [r for r in rooms if r not in NOT_A_DWELLING_ROOM]
        if len(set(rooms)) < min_rooms:
            continue
        low = labels.lower()
        candidates.append({
            "page": number,
            "rooms": sorted(set(rooms)),
            "dwellingRooms": sorted(set(dwelling_rooms)),
            "roomCount": len(set(rooms)),
            "kind": kind_of(page),
            "hasScaleBar": any(w in low for w in ("scale of feet", "seace of feet",
                                                  "scale or feet", "scale of fee")),
            "isDrawing": any(w in low for w in DRAWING_WORDS),
            "image": f"page-{number:03d}.jpg",
        })
    return candidates


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--book", type=Path, default=DEFAULT_BOOK)
    ap.add_argument("--min-rooms", type=int, default=2)
    ap.add_argument("--json", type=Path, default=None)
    ap.add_argument("--min-dimensions", type=int, default=2,
                    help="dimension patterns a specification page must carry")
    args = ap.parse_args()

    if not args.book.exists():
        sys.exit(f"no book at {args.book}")

    pages = parse_book(args.book)
    candidates = triage(pages, args.min_rooms)

    print(f"pages parsed              {len(pages)}")
    print(f"pages with a label block  {sum(1 for p in pages.values() if p['labels'])}")
    print(f"candidates (>= {args.min_rooms} rooms)  {len(candidates)}")
    strong = [c for c in candidates if len(c["dwellingRooms"]) >= 3 and c["isDrawing"]]
    print(f"  of those, a drawing with >= 3 dwelling rooms: {len(strong)}")
    print(f"  of those, carrying a scale bar:               "
          f"{sum(1 for c in strong if c['hasScaleBar'])}")

    specs = specifications(pages, max(args.min_rooms, 4), args.min_dimensions)
    print(f"specification pages (prose programmes): {len(specs)}")
    print("  ", ", ".join(str(s["page"]) for s in specs[:40]))

    freq = Counter(r for c in candidates for r in c["rooms"])
    print("\nroom frequency over the candidates")
    for room, n in freq.most_common(30):
        print(f"  {room:<16} {n}")

    pair = Counter()
    for c in candidates:
        rs = sorted(set(c["rooms"]))
        for i in range(len(rs)):
            for j in range(i + 1, len(rs)):
                pair[(rs[i], rs[j])] += 1
    print("\nco-occurrence, top 20")
    for (a, b), n in pair.most_common(20):
        print(f"  {a:<14} + {b:<14} {n}")

    kinds = Counter(c["kind"] for c in candidates if c["kind"])
    print("\nbuilding kind named on or near the plate")
    for kind, n in kinds.most_common():
        print(f"  {kind:<16} {n}")

    print("\nfirst 30 candidate pages:",
          ", ".join(str(c["page"]) for c in candidates[:30]))

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({
            "minRooms": args.min_rooms,
            "candidates": candidates,
            "specifications": specs,
        }, indent=2) + "\n", encoding="utf-8")
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
