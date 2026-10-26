#!/usr/bin/env python3
"""Append page records to the page manifest.

The programme files under assets/procedural/programmes are the *curated*
training data: one file per design, merged across whatever plates and
specification pages describe it. This manifest is the other thing - one record
per page image of the book, saying what is on that page and what was taken off
it.

Two reasons it exists separately. A sweep of 784 pages has to be resumable and
has to know what it has already looked at, which a set of design files cannot
tell you. And a page that carries no programme still carries information - that
it is a glazing specification, or a plate of mouldings - and recording that is
what stops the same page being read three times.

A record is:
    page     the PDF page number, which is what the image is named by
    kind     plate | specification | prose | table | glossary | blank | front
    title    the heading or plate title, where there is one
    draws    what the plate draws: ground plan, elevation, section, details
    rooms    room labels legible on the page, normalised, with feet where printed
    scale    true when a scale bar is drawn
    design   the programme file this page fed, when it fed one
    note     anything worth saying in a line
"""

import json
import sys
from pathlib import Path

MANIFEST = Path(__file__).resolve().parents[2] / "assets" / "procedural" / "programmes" / "pages.json"


def load():
    if MANIFEST.exists():
        return json.loads(MANIFEST.read_text())

    return {
        "source": "The Builder's Practical Director, 784 page scans",
        "note": "One record per page image. See tools/procedural/write_page.py.",
        "pages": {},
    }


def put(records):
    """records: list of dicts, each with at least `page` and `kind`."""
    manifest = load()

    for record in records:
        page = str(record.pop("page"))
        entry = {k: v for k, v in record.items() if v not in ("", [], None, False)}
        manifest["pages"][page] = entry

    manifest["pages"] = dict(sorted(manifest["pages"].items(), key=lambda kv: int(kv[0])))
    manifest["count"] = len(manifest["pages"])
    MANIFEST.write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")

    return manifest["count"]


def done():
    """Page numbers already recorded."""
    return {int(p) for p in load()["pages"]}


if __name__ == "__main__":
    have = done()
    missing = [p for p in range(1, 785) if p not in have]
    print(f"{len(have)} of 784 pages recorded; {len(missing)} outstanding")

    if missing and "--next" in sys.argv:
        n = int(sys.argv[sys.argv.index("--next") + 1]) if len(sys.argv) > sys.argv.index("--next") + 1 else 8
        print(" ".join(f"{p:03d}" for p in missing[:n]))
