#!/usr/bin/env python3
"""Isolated subdivision comparison using frozen city/land footprints.

Does not run the terrain-gated social pipeline or claim new access acceptance.
"""

import argparse, json, copy
from pathlib import Path
import numpy as np
import shapely as sh
from shapely.geometry import shape, mapping, Point
from shapely.geometry.base import BaseGeometry
from PIL import Image, ImageDraw, ImageFont
from farm_field_subdivision import settlement_scale, subdivide_fields
from compose_island_map import canonical_edges
from render_island_map import render


def encode(o):
    if isinstance(o, BaseGeometry):
        return mapping(o)
    if isinstance(o, np.generic):
        return o.item()
    raise TypeError(type(o).__name__)


def study(source, out):
    original = json.loads(Path(source).read_text())
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    cities = [dict(c, envelope=shape(c["envelope"])) for c in original["cities"]]
    new = copy.deepcopy(original)
    fields = []
    margins = []
    diagnostics = []
    for old in original["fields"]:
        geometry = shape(old["geometry"])
        estate = {
            "id": old["id"],
            "sourceEstate": old["parent"],
            "geometry": geometry,
            "grain": old["grain"],
            "strategy": old["recipe"],
            "settlementScale": settlement_scale(geometry, cities),
        }
        result = subdivide_fields(original["seed"], estate)
        fields.extend(result["fields"])
        margins.extend(result["marginReasons"])
        diagnostics.append(result["diagnostics"])
    new["fields"] = json.loads(json.dumps(fields, default=encode))
    new["fieldEdges"] = json.loads(json.dumps(canonical_edges(fields), default=encode))
    new["farmAccess"] = {
        "status": "must-regenerate-after-subdivision",
        "gates": [],
        "tracks": [],
    }
    new["fieldScaleStudy"] = {
        "source": str(Path(source).resolve()),
        "cityGeometryChanged": False,
        "terrainAdmissionBypassed": False,
        "newSocialPipelineRun": False,
        "accessValidated": False,
        "marginReasons": json.loads(json.dumps(margins, default=encode)),
    }
    oldarea = np.array([shape(f["geometry"]).area for f in original["fields"]])
    newarea = np.array([f["geometry"].area for f in fields])
    metrics = {
        "oldFieldCount": len(oldarea),
        "newFieldCount": len(newarea),
        "oldMedianHectares": float(np.median(oldarea) / 10000),
        "newMedianHectares": float(np.median(newarea) / 10000),
        "oldMaximumHectares": float(oldarea.max() / 10000),
        "newMaximumHectares": float(newarea.max() / 10000),
        "retainedCultivationFraction": float(newarea.sum() / oldarea.sum()),
        "maximumFieldToSettlementAreaRatio": max(
            f["geometry"].area / f["settlementScale"]["settlementAreaMetres2"]
            for f in fields
        ),
        "marginAreaMetres2": sum(m["geometry"].area for m in margins),
        "sourceAreaMetres2": float(oldarea.sum()),
        "diagnostics": diagnostics,
    }
    (out / "field-scale-study.json").write_text(
        json.dumps(new, separators=(",", ":")) + "\n"
    )
    (out / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    # Isolate parcel scale in flat 2D views, without using the obsolete relief.
    before = copy.deepcopy(original)
    after = copy.deepcopy(new)
    for view in (before, after):
        view.pop("terrainSites", None)
        view.pop("landscape", None)
        view["farmAccess"] = {}
    for kind in ("village", "city"):
        candidates = [s for s in original["settlements"] if s["kind"] == kind]
        settlement = max(
            candidates,
            key=lambda s: sum(
                shape(f["geometry"]).intersection(Point(s["position"]).buffer(800)).area
                for f in original["fields"]
            ),
        )
        x, z = settlement["position"]
        span = 850
        bounds = (x - span, z - span, x + span, z + span)
        for name, view in (("before", before), ("after", after)):
            render(
                {},
                view,
                bounds,
                (1100, 1100),
                f"{kind.upper()} / {name.upper()}",
                "Frozen city footprints | field subdivision comparison",
                out / f"{kind}-{name}.png",
            )
        im = Image.new("RGB", (2200, 1210), "#233d3e")
        im.paste(Image.open(out / f"{kind}-before.png"), (0, 0))
        im.paste(Image.open(out / f"{kind}-after.png"), (1100, 0))
        draw = ImageDraw.Draw(im)
        font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 27)
        draw.text(
            (28, 1120),
            f'Same city and map scale | median field {metrics["oldMedianHectares"]:.2f} ha -> {metrics["newMedianHectares"]:.2f} ha | largest {metrics["oldMaximumHectares"]:.2f} ha -> {metrics["newMaximumHectares"]:.2f} ha',
            font=font,
            fill="white",
        )
        draw.text(
            (28, 1160),
            "Subdivision study; new field access requires validation. No new island/city generation.",
            font=font,
            fill="#c5d0c2",
        )
        im.save(out / f"{kind}-field-scale-comparison.png")
    print(
        "PASS field scale comparison:",
        json.dumps({k: v for k, v in metrics.items() if k != "diagnostics"}),
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    parser.add_argument("out")
    args = parser.parse_args()
    study(args.source, args.out)
