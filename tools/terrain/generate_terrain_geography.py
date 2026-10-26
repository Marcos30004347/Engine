"""Generate v10 geography and biome evidence from verified v9 terrain."""

import argparse, copy, hashlib, json, time
from pathlib import Path
import numpy as np
import shapely as sh
from shapely.geometry import shape, mapping, LineString
from terrain_landforms import (
    LandscapeSurface,
    cover_fields,
    cover_polygon,
    sample,
    coherent_noise,
    smooth,
)
from terrain_geography import (
    regional_graph,
    build_geography,
    coastal_bluffs,
    geographic_regions,
    climate_fields,
    biome_partition,
    BIOMES,
    PRESETS,
)


def read_baseline(source):
    source = Path(source)
    manifest = json.loads((source / "manifest.json").read_text())
    required = [
        "terrain-landforms.json",
        "base-height.npy",
        "heightmap-metres.npy",
        "mountain-weight.npy",
        "coast-distance.npy",
        "source/foundation.json",
        "terrain-before-erosion.npy",
        "source/heightmap-metres.npy",
        "source/mountain-weight.npy",
        "source/terrain-map.json",
    ]
    hashes = {}
    for name in required:
        path = source / name
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        matching = [
            v
            for k, v in manifest.items()
            if Path(k).as_posix().endswith("/" + source.name + "/" + name)
        ]
        if matching != [actual]:
            raise ValueError("baseline checksum mismatch: " + name)
        hashes[name] = actual
    j = json.loads((source / "terrain-landforms.json").read_text())
    if not all(j["checks"].values()):
        raise ValueError("baseline not accepted")
    return j, hashes


def write_source_manifest(output, product):
    """Seal the numeric handoff before optional render/test artifacts exist."""
    names = [
        product,
        "base-height.npy",
        "heightmap-metres.npy",
        "coast-distance.npy",
        "mountain-weight.npy",
    ]
    manifest = {
        name: hashlib.sha256((output / name).read_bytes()).hexdigest() for name in names
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def generate(source, output):
    start = time.perf_counter()
    source = Path(source)
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    j, hashes = read_baseline(source)
    f = json.loads((source / "source/foundation.json").read_text())
    h = np.load(source / "base-height.npy")
    weight = np.load(source / "mountain-weight.npy")
    cell = j["surface"]["cellMetres"]
    land = shape(j["land"])
    graph = regional_graph(f, land, h, cell)
    print(
        f"PASS source: verified v9; {len(graph[0])} regional Voronoi owners; {len(graph[-1])} climate interfaces",
        flush=True,
    )
    h, features = build_geography(
        h, cell, land, weight, j["rivers"], j["lakes"], graph, j["seed"]
    )
    coast = np.load(source / "coast-distance.npy")
    h, coastal = coastal_bluffs(h, cell, land, j["rivers"], j["lakes"], coast)
    features += coastal
    surface = LandscapeSurface(h, cell, land, j["rivers"], j["lakes"])
    final = surface.raster(len(h))
    for r in j["rivers"]:
        r["realizedBedMetres"] = surface.sample(r["points"]).tolist()
        r["waterDepthMetres"] = (
            np.array(r["waterSurfaceMetres"]) - r["realizedBedMetres"]
        ).tolist()
    for feature in features:
        if "toe" in feature:
            feature["crestHeightMetres"] = surface.sample(feature["crest"]).tolist()
            feature["toeHeightMetres"] = surface.sample(feature["toe"]).tolist()
    coast = np.load(source / "coast-distance.npy")
    covers = cover_fields(
        final, cell, land, coast, j["cliffs"], j["rivers"], j["lakes"], season="summer"
    )
    fields, regions = geographic_regions(
        final, cell, land, weight, j["rivers"], j["lakes"], features, j["cliffs"]
    )
    x, z = np.meshgrid(np.arange(len(h)) * cell, np.arange(len(h)) * cell)
    mask = sh.contains_xy(land, x, z)
    water = sh.union_all(
        [shape(l["geometry"]) for l in j["lakes"]]
        + [LineString(r["points"]).buffer(r["width"] / 2) for r in j["rivers"]]
    )
    # Rock exposures must come from physical slopes/features, not a painted cliff.
    cover_rock = np.maximum(covers["rock"], fields["scree"] * 0.6)
    covers["sand"] *= 1 - smooth((covers["slope"] - 0.12) / 0.20)
    graph = regional_graph(f, land, final, cell)
    product = copy.deepcopy(j)
    product.update(
        stage="terrain-geography-v10",
        source=str(source),
        sourceHashes=hashes,
        geography=dict(
            features=features,
            regions=regions,
            existingInlandCliffs=j["cliffs"],
            protectedWaterBufferMetres=110,
            methods="planning-domain spline caps, coastal bluffs, resistant spur outcrops; channel-relative valleys",
        ),
        environment=dict(
            climateGraph=dict(
                regionIds=[r["id"] for r in graph[0]],
                centres=graph[2].tolist(),
                areasMetres2=graph[3].tolist(),
                heightsMetres=graph[4].tolist(),
                edges=[list(e) for e in graph[5]],
                wind=[1, 0],
            ),
            presets={},
        ),
        acceptance=dict(
            terrainOnlyChecksPassed=False,
            settlementReady=False,
            climateHydrologyCoupled=False,
            dynamicBackwaterSolved=False,
        ),
        pending=[
            "Climate runoff must be reconciled with river discharge before settlement admission",
            "Multi-seed whole-island qualification; runtime integration",
            "Dynamic floods/backwater; aeolian dune transport, glaciers and caves",
        ],
    )
    product.pop("ridgeSections", None)
    product.pop("metrics", None)
    product.pop("checks", None)
    product["surface"][
        "type"
    ] = "SIGGRAPH watershed terrain plus spline geology and canonical water primitives"
    product["climate"] = dict(
        preset="mixed-showcase",
        stylized=True,
        annualClimateProxy=True,
        hydrologyRecoupled=False,
    )
    np.save(output / "base-height.npy", h)
    np.save(output / "heightmap-metres.npy", final)
    np.save(output / "mountain-weight.npy", weight)
    np.save(output / "coast-distance.npy", coast)
    for name, a in fields.items():
        np.save(output / (name + ".npy"), a)
    for name, a in covers.items():
        np.save(output / (name + ".npy"), a)
    np.save(output / "rock.npy", cover_rock)
    for preset in PRESETS:
        arrays, metadata = climate_fields(graph, final, cell, mask, preset, j["seed"])
        partition = biome_partition(arrays["biome_weights"], cell, land, water)
        metadata["biomes"] = partition
        metadata["biomeAreaMetres2"] = {p["id"]: p["areaMetres2"] for p in partition}
        metadata["precipitationRangeMm"] = [
            float(arrays["precipitation"][mask].min()),
            float(arrays["precipitation"][mask].max()),
        ]
        product["environment"]["presets"][preset] = metadata
        target = output / preset
        target.mkdir(exist_ok=True)
        for name, a in arrays.items():
            np.save(target / (name.replace("_", "-") + ".npy"), a)
        if preset == "mixed-showcase":
            np.save(output / "biome-weights.npy", arrays["biome_weights"])
            np.save(output / "temperature.npy", arrays["temperature"])
            forest = arrays["biome_weights"][3] + arrays["biome_weights"][4]
            canopy = forest * (
                0.72 + 0.28 * smooth(coherent_noise(x, z, 95, j["seed"] + 100))
            )
            canopy[covers["waterMask"]] = 0
            np.save(output / "canopy.npy", canopy)
        print(
            "PASS climate "
            + preset
            + ": "
            + json.dumps(metadata["biomeAreaMetres2"])
            + "; moisture residual="
            + str(metadata["atmosphericLedger"]["residual"]),
            flush=True,
        )
    product["covers"] = {
        key: mapping(cover_polygon(np.load(output / (key + ".npy")), cell, land))
        for key in ["sand", "snow", "rock"]
    }
    checks = dict(
        finiteHeight=bool(np.isfinite(final).all()),
        plateaus=sum(t["type"] == "plateau" for t in features) == 2,
        coastalCliffs=len(coastal) == 2,
        sourceHeights=all(
            surface.sample([s["position"]])[0] >= 70 for s in j["sources"]
        ),
        wetRiverStations=all(min(r["waterDepthMetres"]) >= 0 for r in j["rivers"]),
        canonicalCoast=product["land"] == j["land"],
        lakePortsWet=all(
            np.max(surface.sample([l["inletPosition"], l["outletPosition"]]))
            < l["waterLevelMetres"]
            for l in j["lakes"]
        ),
    )
    for key, value in checks.items():
        if not value:
            raise ValueError("geography acceptance failed: " + key)
    product["checks"] = {k: bool(v) for k, v in checks.items()}
    product["acceptance"]["terrainOnlyChecksPassed"] = True
    product["metrics"] = dict(
        peakMetres=float(final.max()),
        featureCounts={
            t: sum(p["type"] == t for p in features)
            for t in sorted({p["type"] for p in features})
        },
        highGroundSources=len(j["sources"]),
        oceanMouths=sum(r["terminal"] == "ocean" for r in j["rivers"]),
        connectedLakes=len(j["lakes"]),
        seconds=time.perf_counter() - start,
    )
    (output / "terrain-geography.json").write_text(json.dumps(product, indent=2) + "\n")
    write_source_manifest(output, "terrain-geography.json")
    print("PASS geography: " + json.dumps(product["metrics"]), flush=True)
    return product


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("source")
    p.add_argument("output")
    a = p.parse_args()
    generate(a.source, a.output)
