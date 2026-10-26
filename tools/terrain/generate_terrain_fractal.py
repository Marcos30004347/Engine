"""Fresh fractal island → hydraulic erosion → climate/detail; rivers deferred."""

import argparse, json, time, hashlib
from pathlib import Path
import numpy as np
import shapely as sh
from shapely.geometry import mapping, GeometryCollection
from terrain_fractal_world import construct_height
from terrain_feature_scale import scale_contract
from terrain_landforms import erode_slopes, cover_fields, cover_polygon, sample
from terrain_geography import regional_graph, biome_partition
from terrain_regional_weather import RECIPES, raster_climate
from terrain_regional_detail import desert_detail, geology_clearance, coordinates


def generate(
    output, seed=27, side=1025, extent=6000.0, binary="build/tests/WorldTerrainErosion"
):
    start = time.perf_counter()
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    pre, w, erodibility, land, coast, planning, features, domains, cell = (
        construct_height(seed, side, extent)
    )
    x, z, q = coordinates(pre, cell)
    mask = sh.contains_xy(land, x, z)
    print(
        "PASS fresh fractal terrain: "
        + str(len(domains))
        + " uplift domains; no previous terrain/river inputs",
        flush=True,
    )
    unconstrained = np.full_like(pre, 1e6)
    h, erosion = erode_slopes(
        pre,
        cell,
        erodibility,
        unconstrained,
        unconstrained,
        mask,
        seed + 200,
        binary,
        crest_protection=0,
    )
    delta = pre - h
    erosion["scope"] = "all eligible land; no river or watershed-crest reservations"
    erosion["changedAreaMetres2"] = float(np.count_nonzero(abs(delta) > 0.05) * cell**2)
    covers = cover_fields(h, cell, land, coast, [], [], [], season="summer")
    graph = regional_graph(planning, land, h, cell)
    product = dict(
        schema=1,
        stage="terrain-physical-scale-v17",
        terrainMethod="fBm landmass + feedback ridged multifractal relief + hydraulic erosion",
        seed=seed,
        extent=extent,
        land=mapping(land),
        rivers=[],
        lakes=[],
        sources=[],
        cliffs=[],
        cliffsEnabled=False,
        hydrology=dict(
            status="deferred", riverGraphUsedForTerrain=False, lakeBasinsSolved=False
        ),
        construction=dict(
            inheritsPreviousTerrain=False,
            usesWatershedEdges=False,
            usesSiggraph2013=False,
            noiseBasis="world-coordinate quintic gradient noise",
            landmass="fBm plus island falloff and sea threshold",
            mountains="feedback ridged multifractal",
            octaves=5,
            lacunarity=2.0,
            H=1.0,
            offset=1.0,
            gain=2.0,
            wavelengthMetres=950,
            domainWarp=True,
            upliftDomains=domains,
        ),
        surface=dict(
            type="2D scalar terrain before hydrology",
            cellMetres=cell,
            side=side,
            rowOrder="south-to-north",
            raster="heightmap-metres.npy",
            presetTerrain="<preset>/heightmap-metres.npy",
        ),
        geography=dict(features=features),
        physicalScale=scale_contract(cell),
        erosion=erosion,
        environment=dict(
            presets={},
            climateGraph=dict(
                regionIds=[r["id"] for r in graph[0]],
                centres=graph[2].tolist(),
                areasMetres2=graph[3].tolist(),
                heightsMetres=graph[4].tolist(),
                edges=graph[5],
            ),
        ),
        desertDetail={},
        acceptance=dict(
            terrainOnlyChecksPassed=False, settlementReady=False, hydrologyReady=False
        ),
        meshing=False,
        socialSolversRun=False,
        pending=[
            "Visual qualification across full-island seeds",
            "Generate rivers and lakes from accepted terrain; resolve depressions and ocean outlets",
            "Couple climate runoff and accumulated river discharge",
            "Generate roads and settlements after hydrology",
        ],
    )
    clear = geology_clearance(features, q, pad=30).reshape(h.shape)
    for preset in RECIPES:
        arrays, meta = raster_climate(graph, h, cell, mask, coast, preset)
        meta["biomes"] = biome_partition(
            arrays["biome_weights"], cell, land, GeometryCollection()
        )
        meta["biomeAreaMetres2"] = {b["id"]: b["areaMetres2"] for b in meta["biomes"]}
        folder = output / preset
        folder.mkdir(exist_ok=True)
        for key, a in arrays.items():
            np.save(folder / (key.replace("_", "-") + ".npy"), a)
        wind = arrays["wind"][:, mask].mean(axis=1)
        dunes, eligibility, dune_meta = desert_detail(
            h, cell, arrays["biome_weights"][0], wind, coast, unconstrained, clear, seed
        )
        for name, a in [
            ("dune-height", dunes),
            ("dune-weight", eligibility),
            ("heightmap-metres", h + dunes),
        ]:
            np.save(folder / (name + ".npy"), a)
        product["environment"]["presets"][preset] = meta
        product["desertDetail"][preset] = dune_meta
        print(
            "PASS climate/detail " + preset + ": " + json.dumps(dune_meta), flush=True
        )
    for name, a in dict(
        terrain_before_erosion=pre,
        base_height=h,
        heightmap_metres=h,
        mountain_weight=w,
        erosion_weight=erodibility,
        coast_distance=coast,
        erosion_delta_metres=delta,
        **covers
    ).items():
        np.save(output / (name.replace("_", "-") + ".npy"), a)
    product["covers"] = {
        name: mapping(cover_polygon(covers[name], cell, land))
        for name in ["sand", "rock", "snow"]
    }
    product["metrics"] = dict(
        peakMetres=float(h.max()),
        minimumLandHeightMetres=float(h[mask].min()),
        landAreaMetres2=float(land.area),
        mountainAreaMetres2=float(np.count_nonzero((w > 0.3) & mask) * cell**2),
        planningRegions=len(graph[0]),
        plateaus=len(features),
        erosionChangedAreaMetres2=erosion["changedAreaMetres2"],
        seconds=time.perf_counter() - start,
    )
    print("SCALE " + json.dumps(product["physicalScale"]), flush=True)
    product["checks"] = dict(
        finiteHeight=bool(np.isfinite(h).all()),
        seaUnchanged=bool(np.all(h[~mask] == 0)),
        nonnegativeLand=bool(np.min(h[mask]) >= -1e-8),
        noInheritedWater=True,
        noDedicatedCliffs=True,
        sedimentConserved=bool(
            all(
                abs(e["massResidualCubicMetres"])
                < 1e-7 * max(1, e["erodedCubicMetres"])
                for e in erosion["passes"]
            )
        ),
    )
    if not all(product["checks"].values()):
        raise ValueError("terrain-first gate failed: " + str(product["checks"]))
    product["acceptance"]["terrainOnlyChecksPassed"] = True
    (output / "terrain-world.json").write_text(json.dumps(product, indent=2) + "\n")
    (output / "planning-map.json").write_text(json.dumps(planning, indent=2) + "\n")
    (output / "manifest.json").write_text(
        json.dumps(
            {
                str(p.relative_to(output)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in output.rglob("*")
                if p.is_file() and p.name != "manifest.json"
            },
            indent=2,
        )
        + "\n"
    )
    print("PASS terrain-first island: " + json.dumps(product["metrics"]), flush=True)
    return product


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("output")
    p.add_argument("--seed", type=int, default=27)
    p.add_argument("--side", type=int, default=1025)
    p.add_argument("--extent", type=float, default=6000)
    p.add_argument("--erosion-binary", default="build/tests/WorldTerrainErosion")
    a = p.parse_args()
    generate(a.output, a.seed, a.side, a.extent, a.erosion_binary)
