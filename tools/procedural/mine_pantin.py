#!/usr/bin/env python3
"""Mine W. A. Pantin's 'Medieval English Town-House Plans' into programme JSON.

A second corpus, and a different one. The builder's book is Victorian, and its
houses are designed - a catalogue of what a builder should put up. Pantin's
forty houses are medieval and surveyed: what was actually standing in Oxford,
Coventry, Bristol, Norwich, Exeter, King's Lynn and Canterbury in the
fourteenth and fifteenth centuries, measured by archaeologists.

Two things make it worth having beside the other. It carries a plan-type
taxonomy that the Victorian book has no equivalent of - whether the hall lies
parallel to the street or at right angles to it, and within each, how the plan
was squeezed onto a town plot - which is exactly the axis a massing variable
needs. And it is a town corpus: every house is on a burgage plot with a
frontage, which is the constraint a town generator actually works under.

Unlike the builder's book this one has a text layer, so the catalogue and its
dimensions come out by parsing rather than by eye. The figures still have to be
read as images to get the room arrangement.

    pdftotext -layout buildingsengland.pdf england.txt
    mine_pantin.py england.txt
"""

import json
import re
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parents[2] / "assets" / "procedural" / "programmes" / "medieval"

# The paper's own classification, in the order its figure captions give it.
TYPES = [
    ("I. 'Parallel' type: i. 'Extended' plan", "parallel-extended"),
    ("I. 'Parallel' type: ii. 'Contracted' plan", "parallel-contracted"),
    ("I. 'Parallel' type: iii.", "parallel-third"),
    ("I. 'Parallel' type: iv. 'Courtyard' plan", "parallel-courtyard"),
    ("II. 'Right-angle' type: i. 'Narrow' plan", "right-angle-narrow"),
    ("II. 'Right-angle' type: ii. 'Broad' plan", "right-angle-broad"),
]

# Medieval room vocabulary. The normalised name is on the right; where the
# Victorian corpus has the same room under another word, it uses that word, so
# the two corpora can be counted together.
VOCABULARY = {
    "screens-passage": "screens_passage",
    "screens passage": "screens_passage",
    "service-room": "service_room",
    "service room": "service_room",
    "counting-house": "office",
    "counting house": "office",
    "undercroft": "cellar",
    "warehouse": "warehouse",
    "courtyard": "court",
    "garderobe": "wc",
    "buttery": "buttery",
    "pantry": "pantry",
    "kitchen": "kitchen",
    "parlour": "parlour",
    "chamber": "chamber",
    "solar": "solar",
    "gallery": "gallery",
    "passage": "passage",
    "cellar": "cellar",
    "shop": "shop",
    "stair": "staircase",
    "hall": "hall",
}

# The catalogue numbers each house and follows it with a name, a town and
# usually a figure reference and a footnote marker, all of which the OCR
# mangles differently every time - so take the line loosely and clean after.
ENTRY_RE = re.compile(r"^\s*(\d{1,2})\.\s{1,5}(\S.{3,70}?)\s*$")

# "25 ft. by 19 ft.", "27 ft. by 25 ft. wide", "15; ft. by 2 I ft." - the OCR is
# clean enough here that the numbers survive, unlike the builder's book's
# feet-and-inches marks.
DIM_RE = re.compile(r"(\d{1,3})\s*(?:[;.]\s*\d)?\s*ft\.?\s*(?:wide\s+|long\s+|deep\s+)?"
                    r"(?:by|x|×)\s*(?:c\.\s*)?(\d{1,3})", re.IGNORECASE)


def clean(name):
    """Strip the figure reference and the footnote marker off an entry name."""
    name = re.sub(r"\s*\(FIG[^)]*\)?", "", name, flags=re.IGNORECASE)
    name = re.sub(r"[.,;:\s]*[A-Za-z0-9'\"~*^]{0,4}$", "", name) if name.endswith(
        tuple("\"'~*^")) else name
    return re.sub(r"\s+", " ", name).strip(" .,;:")


def rooms_near(sentence):
    """Room words named in a sentence, longest spelling first."""
    low = " " + sentence.lower() + " "
    found = []

    for spelling in sorted(VOCABULARY, key=len, reverse=True):
        if re.search(r"(?<![a-z])" + re.escape(spelling) + r"(?![a-z])", low):
            found.append(VOCABULARY[spelling])
            low = low.replace(spelling, " ")

    return found


def main():
    text = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()

    # Which plan type is in force at each line, from the figure captions.
    type_at = {}
    current = ""

    for i, line in enumerate(lines):
        squashed = re.sub(r"\s+", " ", line).strip()

        # The captions are OCR'd loosely - "typc" for "type", "11." for "II." -
        # so key on the distinctive word rather than the whole phrase. Each of
        # the five names appears only in its own caption.
        low = squashed.lower()

        if "plan" in low and ("pp." in low or "p." in low):
            for word, slug in (("extended", "parallel-extended"),
                               ("contracted", "parallel-contracted"),
                               ("courtyard", "parallel-courtyard"),
                               ("narrow", "right-angle-narrow"),
                               ("broad", "right-angle-broad")):
                if word in low:
                    current = slug

        type_at[i] = current

    # Split into catalogue entries.
    starts = []

    for i, line in enumerate(lines):
        m = ENTRY_RE.match(line)

        if m and 1 <= int(m.group(1)) <= 40:
            starts.append((i, int(m.group(1)), m.group(2).strip()))

    # Section headings and figure captions look like catalogue entries to a
    # loose line pattern - "11. THE 'RIGHT-ANGLE' TYPE" reads as entry 11. The
    # catalogue numbers ascend, so anything that goes backwards is not an entry.
    ascending, highest = [], 0

    for entry in starts:
        if entry[1] > highest and "TYPE" not in entry[2] and "plan (p" not in entry[2]:
            ascending.append(entry)
            highest = entry[1]

    starts = ascending

    entries = []

    for k, (i, number, name) in enumerate(starts):
        end = starts[k + 1][0] if k + 1 < len(starts) else len(lines)
        body = "\n".join(lines[i:end])

        # The type is whichever caption most recently preceded the entry; the
        # captions sit on the figure pages, which interleave with the text.
        plan_type = type_at.get(i, "")

        rooms = []
        flat = re.sub(r"\s+", " ", body)

        # Sentences are no use here: "31 ft. by 17 ft." contains two full stops
        # and any sentence splitter cuts the dimension in half. So take a window
        # of text around each measurement and read the room out of that - the
        # room a measurement belongs to is almost always the noun just before it
        # ("an open hall, about 31 ft. by 17 ft."), so the window leans backward.
        for dm in DIM_RE.finditer(flat):
            a, b = int(dm.group(1)), int(dm.group(2))

            if not (5 <= a <= 120 and 5 <= b <= 120):
                continue

            window = flat[max(0, dm.start() - 110):dm.end() + 40]
            before = flat[max(0, dm.start() - 110):dm.start()]

            # Prefer a room named before the measurement; fall back to the
            # whole window.
            near = rooms_near(before) or rooms_near(window)

            if near:
                rooms.append({"use": near[-1] if rooms_near(before) else near[0],
                              "feet": [max(a, b), min(a, b)],
                              "raw": window.strip()[:170]})

        mentioned = sorted(set(r for s in [body] for r in rooms_near(s)))

        entries.append({
            "number": number,
            "name": clean(name),
            "planType": plan_type,
            "roomsMentioned": mentioned,
            "dimensioned": rooms,
        })

    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "pantin-catalogue.json").write_text(json.dumps({
        "source": "W. A. Pantin, 'Medieval English Town-House Plans', "
                  "Medieval Archaeology, from buildingsengland.pdf",
        "note": "Mined from the PDF text layer; the figures still need reading as images.",
        "types": {slug: caption for caption, slug in TYPES},
        "houses": entries,
    }, indent=1) + "\n", encoding="utf-8")

    withdims = sum(1 for e in entries if e["dimensioned"])
    print(f"{len(entries)} houses, {withdims} with at least one dimensioned room, "
          f"{sum(len(e['dimensioned']) for e in entries)} dimensioned rooms")

    from collections import Counter
    print("plan types:", dict(Counter(e["planType"] or "unassigned" for e in entries)))
    print("rooms:", dict(Counter(r for e in entries for r in e["roomsMentioned"]).most_common(20)))


if __name__ == "__main__":
    main()
