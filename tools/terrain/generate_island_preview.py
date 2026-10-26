#!/usr/bin/env python3
"""Assemble the latest terrain and regional recipes into a full 2D draft."""

import argparse, time
from pathlib import Path
from island_preview_handoff import prepare_preview
from compose_island_map import compose, export_product

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("terrain")
    parser.add_argument("output")
    parser.add_argument("--workers", type=int, default=4)
    args = parser.parse_args()
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    start = time.perf_counter()
    foundation = prepare_preview(args.terrain)
    print(
        "PASS preview handoff: source hashes, exact scalar terrain, drainage and all basin reservations",
        flush=True,
    )
    island, product, trace = compose(foundation, args.workers, preview=True)
    product["pending"] += [
        "physical lake outlet/backwater validation",
        "adaptive terrain/river agreement",
        "multi-seed integrated visual qualification",
    ]
    product["landscape"]["hydraulicSectionsRequireRefresh"] = True
    export_product(out / "island-map.json", product)
    export_product(out / "foundation.json", foundation)
    export_product(
        out / "solver-trace.json",
        {
            "seconds": time.perf_counter() - start,
            "workers": args.workers,
            "barriers": trace,
        },
    )
    print(
        f'PASS full 2D preview: {len(product["settlements"])} settlements, {len(product["roads"])} connecting roads, {len(product["fields"])} fields, {len(product["farmAccess"]["gates"])} connected field gates; {time.perf_counter()-start:.2f}s',
        flush=True,
    )
