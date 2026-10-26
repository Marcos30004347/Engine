#!/usr/bin/env python3
"""Classify every page of the book from its OCR, to aim the vision pass.

Seven hundred and eighty-four page images is too many to read one at a time
without a reason for each. The OCR cannot give the dimensions - that is the
whole finding behind this corpus - but it can say reliably which pages carry a
drawing at all, because the extraction wrote an image description for every
page and those descriptions distinguish a plate from a page of prose.

So: classify everything from the text, then read as images only the pages that
have something drawn on them. The classification is recorded with the page so
it is clear which pages were judged by their OCR and which were looked at.
"""

import json
import re
import sys
from pathlib import Path

BOOK = Path.home() / "Desktop" / "building-book-markdown-and-images (1)" / "building-book.md"

PAGE_RE = re.compile(r"^## PDF page (\d+)\s*$")

# What the extraction's own image descriptions say about a page.
DRAWING_HINTS = ("architectural drawing page", "illustration page showing a drawing",
                 "diagram plate", "showing floor plan", "showing plan", "plate showing")
PROSE_HINTS = ("scanned prose page", "primarily textual", "arranged primarily as text")
TABLE_HINTS = ("or a table", "table of", "quantities, prices")


def parse():
    pages = {}
    current = None
    mode = None

    for line in BOOK.read_text(encoding="utf-8", errors="replace").splitlines():
        m = PAGE_RE.match(line)

        if m:
            current = {"page": int(m.group(1)), "desc": "", "labels": [], "text": []}
            pages[current["page"]] = current
            mode = None

            continue

        if current is None:
            continue

        if line.startswith("**Image description:**"):
            current["desc"] = line[len("**Image description:**"):].strip()
        elif line.startswith("#### Supplemental drawing labels"):
            mode = "labels"
        elif line.startswith("### Extracted text"):
            mode = "text"
        elif line.startswith("#"):
            mode = None
        elif mode == "labels" and line.strip():
            current["labels"].append(line.strip())
        elif mode == "text":
            current["text"].append(line)

    return pages


def classify(page):
    desc = page["desc"].lower()
    labels = " ".join(page["labels"]).lower()
    text = "\n".join(page["text"])

    drawing = any(h in desc for h in DRAWING_HINTS)
    plan = "plan" in labels or "plan" in desc
    elevation = "elevation" in labels or "elevation" in desc
    section = "section" in labels
    scale = any(s in labels for s in ("scale of feet", "seace of feet", "scale or feet"))

    if drawing or (labels and (plan or elevation)):
        kind = "plate"
    elif any(h in desc for h in TABLE_HINTS):
        kind = "table"
    elif any(h in desc for h in PROSE_HINTS) or len(text) > 400:
        kind = "prose"
    elif not text.strip() and not labels.strip():
        kind = "blank"
    else:
        kind = "prose"

    draws = []

    for name, flag in (("plan", plan), ("elevation", elevation), ("section", section)):
        if flag:
            draws.append(name)

    return {
        "kind": kind,
        "draws": draws,
        "scale": scale,
        # A plate with plan or elevation in its labels is worth a vision pass;
        # so is any page the description calls a drawing.
        "read": kind == "plate",
    }


def main():
    pages = parse()
    out = {}

    for number in sorted(pages):
        out[number] = classify(pages[number])

    counts = {}

    for entry in out.values():
        counts[entry["kind"]] = counts.get(entry["kind"], 0) + 1

    print("classified", len(out), "pages:", counts)
    print("worth a vision pass:", sum(1 for e in out.values() if e["read"]))

    if "--json" in sys.argv:
        path = Path(sys.argv[sys.argv.index("--json") + 1])
        path.write_text(json.dumps(out, indent=1) + "\n")
        print("wrote", path)


if __name__ == "__main__":
    main()
