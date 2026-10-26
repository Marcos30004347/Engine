#!/usr/bin/env python3
"""Print the OCR drawing labels and the image description for given pages.

A reading aid for the Stage A transcription: the labels say what to look for
on the scan, and the description warns where the OCR is unsure. Neither is
authoritative - the plate is - but a label block that names a room the scan
does not show is a sign the page is a specification rather than a plan.
"""

import sys
from pathlib import Path

BOOK = Path.home() / "Desktop" / "building-book-markdown-and-images (1)" / "building-book.md"


def main():
    pages = {int(a) for a in sys.argv[1:]}
    lines = BOOK.read_text(encoding="utf-8", errors="replace").splitlines()

    current, mode = None, None

    for line in lines:
        if line.startswith("## PDF page "):
            current = int(line.rsplit(" ", 1)[1])
            mode = None

            if current in pages:
                print(f"\n===== page {current} =====")

            continue

        if current not in pages:
            continue

        if line.startswith("**Image description:**"):
            print("DESC:", line[len("**Image description:**"):].strip()[:400])
        elif line.startswith("#### Supplemental drawing labels"):
            mode = "labels"
            print("LABELS:")
        elif line.startswith("#"):
            mode = None
        elif mode == "labels" and line.strip():
            print("   ", line.strip())


if __name__ == "__main__":
    main()
