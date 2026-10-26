"""Recompute only the dependent desert overlay from exported geology and climate."""

import argparse, json, hashlib
from pathlib import Path
import numpy as np
import shapely as sh
from shapely.geometry import shape, LineString
from terrain_regional_detail import coordinates, desert_detail, geology_clearance


def refresh(directory):
    root = Path(directory)
    manifest = json.loads((root / "manifest.json").read_text())
    required = [
        "terrain-weather.json",
        "heightmap-metres.npy",
        "coast-distance.npy",
    ] + [
        p + "/" + n + ".npy"
        for p in ["temperate", "arid", "humid-tropical"]
        for n in ["biome-weights", "wind"]
    ]
    for name in required:
        if manifest.get(name) != hashlib.sha256((root / name).read_bytes()).hexdigest():
            raise ValueError("desert input checksum mismatch: " + name)
    j = json.loads((root / "terrain-weather.json").read_text())
    h = np.load(root / "heightmap-metres.npy")
    coast = np.load(root / "coast-distance.npy")
    cell = j["surface"]["cellMetres"]
    x, z, q = coordinates(h, cell)
    mask = sh.contains_xy(shape(j["land"]), x, z)
    water = sh.union_all(
        [LineString(r["points"]).buffer(r["width"] / 2) for r in j["rivers"]]
        + [shape(l["geometry"]) for l in j["lakes"]]
    )
    distance = sh.distance(sh.points(q), water).reshape(h.shape)
    clear = geology_clearance(j["geography"]["features"], q).reshape(h.shape)
    for preset in ["temperate", "humid-tropical", "arid"]:
        folder = root / preset
        weights = np.load(folder / "biome-weights.npy")
        wind = np.load(folder / "wind.npy")[:, mask].mean(axis=1)
        delta, dune_weight, meta = desert_detail(
            h, cell, weights[0], wind, coast, distance, clear, j["seed"]
        )
        np.save(folder / "dune-height.npy", delta)
        np.save(folder / "dune-weight.npy", dune_weight)
        np.save(folder / "heightmap-metres.npy", h + delta)
        j["desertDetail"][preset] = meta
        print(
            "PASS refreshed desert detail " + preset + ": " + json.dumps(meta),
            flush=True,
        )
    (root / "terrain-weather.json").write_text(json.dumps(j, indent=2) + "\n")
    # Update only this stage's outputs, preserving all recorded upstream hashes.
    for name in ["terrain-weather.json"] + [
        p + "/" + n + ".npy"
        for p in ["temperate", "arid", "humid-tropical"]
        for n in ["dune-height", "dune-weight", "heightmap-metres"]
    ]:
        manifest[name] = hashlib.sha256((root / name).read_bytes()).hexdigest()
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("directory")
    a = p.parse_args()
    refresh(a.directory)
