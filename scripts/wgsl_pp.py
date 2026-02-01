#!/usr/bin/env python3
import sys
import io
from pathlib import Path
from pcpp import Preprocessor

def main():
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("src")
    parser.add_argument("-o", "--out", required=True)
    parser.add_argument("-I", action="append", default=[])
    parser.add_argument("-D", action="append", default=[])
    args = parser.parse_args()

    pp = Preprocessor()
    pp.line_directive = None

    for inc in args.I:
        pp.add_path(inc)

    for d in args.D:
        if "=" in d:
            k, v = d.split("=", 1)
            pp.define(f"{k} {v}")
        else:
            pp.define(d)

    src = Path(args.src)
    with src.open("r", encoding="utf-8") as f:
        pp.parse(f.read(), str(src))

    # Write to buffer first
    buf = io.StringIO()
    pp.write(buf)

    # Strip empty / whitespace-only lines
    processed = "\n".join(
        line for line in buf.getvalue().splitlines()
        if line.strip()
    )

    with open(args.out, "w", encoding="utf-8") as out:
        out.write(processed + "\n")

if __name__ == "__main__":
    main()
