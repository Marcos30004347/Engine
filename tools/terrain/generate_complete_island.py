"""48 km 2D geographic island: geology, erosion, climate, lakes and rivers."""

import argparse, hashlib, json, time
from pathlib import Path
import numpy as np
import shapely as sh
from shapely.geometry import mapping, GeometryCollection, LineString, shape
from scipy.spatial import cKDTree
from terrain_large_island import build_terrain, climate
from terrain_island_drainage import build_drainage
from terrain_landforms import erode_slopes, smooth, cover_polygon
from terrain_geography import biome_partition, BIOMES
from terrain_feature_scale import scale_contract


def generate(output, seed=27, side=1025):
    root = Path(output)
    root.mkdir(parents=True, exist_ok=True)
    start = time.perf_counter()
    pre, w, land, mask, coast, cell, planning, domains = build_terrain(seed, side)
    print(
        f"PASS rugged island geology: extent=48000m, peak={pre.max():.1f}m, cell={cell:g}m",
        flush=True,
    )
    dz, dx = np.gradient(pre, cell)
    erodibility = (
        np.maximum(0.22 + 0.25 * smooth(np.hypot(dx, dz) / 0.35), w)
        * mask
        * smooth(coast / 200)
        * smooth(pre / 15)
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
        "build/tests/WorldTerrainErosionRegional",
        crest_protection=0,
        erosion_settings=dict(
            dropletsPerCell=8,
            maximumSteps=110,
            maximumCutMetres=50,
            maximumDepositMetres=6,
        ),
    )
    np.save(root / "terrain-before-erosion.npy", pre)
    np.save(root / "eroded-geology.npy", h)
    arrays, weather = climate(h, mask, cell)
    print(
        "PASS seasonal climate: " + json.dumps(weather["rainfallRangeMm"]), flush=True
    )
    water, drainage = build_drainage(h, mask, cell, land, arrays["runoff_mm"])
    print("PASS terrain-derived drainage: " + json.dumps(water["metrics"]), flush=True)
    final = drainage["conditioned_height"].copy()
    # Export continuous surface profiles separately from geometry triangulation.
    # Curve samples follow monotone arc-length interpolation of routed levels.
    probes = []
    levels = []
    widths = []
    depths = []
    for r in water["rivers"]:
        points = np.array(r["points"])
        raw = np.array(r["routingPoints"])
        arc = np.r_[0, np.cumsum(np.linalg.norm(np.diff(points, axis=0), axis=1))]
        old_arc = np.r_[0, np.cumsum(np.linalg.norm(np.diff(raw, axis=0), axis=1))]
        surface = np.interp(
            arc / max(arc[-1], 1) * old_arc[-1], old_arc, r["waterProfileMetres"]
        )
        r["surfaceProfileMetres"] = surface.tolist()
        r["waterGeometry"] = mapping(
            LineString(points).buffer(r["width"] / 2, cap_style=1)
        )
        probes.extend(points)
        levels.extend(surface)
        widths.extend([r["width"]] * len(points))
        depths.extend([r["depth"]] * len(points))
    axis = np.arange(len(h)) * cell
    x, z = np.meshgrid(axis, axis)
    dist, ids = cKDTree(probes).query(np.column_stack([x.ravel(), z.ravel()]))
    width = np.array(widths)[ids]
    depth = np.array(depths)[ids]
    surface = np.array(levels)[ids]
    channel = (
        smooth((width / 2 + 12 - dist) / 12).reshape(h.shape)
        * mask
        * (drainage["lake_id"] == 0)
    )
    bed = (surface - depth).reshape(h.shape)
    final = np.where(
        channel > 0, np.minimum(final, final * (1 - channel) + bed * channel), final
    )
    # Keep the current small-dune contract. The island scalar is too coarse for
    # 40 m dunes; regional world-coordinate queries supply those later.
    arrays["dune_height"] = np.zeros_like(h)
    arrays["dune_weight"] = np.zeros_like(h)
    water_mask = drainage["lake_id"] > 0
    dz, dx = np.gradient(final, cell)
    slope = np.hypot(dx, dz)
    sand = (
        (1 - smooth((coast - 25) / 32))
        * (1 - smooth((final - 2) / 3))
        * (1 - smooth((slope - 0.1) / 0.18))
        * mask
        * (~water_mask)
    )
    rock = smooth((slope - 0.28) / 0.5) * smooth((h - 180) / 400) * mask * (~water_mask)
    snow = np.zeros_like(h)  # tropical summit is not automatically permanent snow
    lake_geo = (
        sh.union_all([shape(l["geometry"]) for l in water["lakes"]])
        if water["lakes"]
        else GeometryCollection()
    )
    climate_metadata = weather | dict(
        biomes=biome_partition(arrays["biome_weights"], cell, land, lake_geo)
    )
    product = dict(
        schema=1,
        stage="complete-geographic-island-v18",
        seed=seed,
        extent=48000,
        land=mapping(land),
        terrainMethod="explicit rugged-island ridged multifractals + regional particle erosion",
        construction=dict(
            usesSiggraph2013=False,
            usesWatershedEdges=False,
            inheritsPreviousTerrain=False,
            upliftDomains=domains,
        ),
        surface=dict(
            type="2D scalar and vector geography",
            cellMetres=cell,
            side=side,
            rowOrder="south-to-north",
            raster="heightmap-metres.npy",
        ),
        rivers=water["rivers"],
        lakes=water["lakes"],
        sources=water["sources"],
        riverNodes=water["nodes"],
        cliffs=[],
        cliffsEnabled=False,
        geography=dict(features=[]),
        hydrology=dict(
            status="terrain-derived static drainage",
            routing="Priority-Flood + steepest descent and acyclic flat routing",
            runoff="annual P-AET catchment yield, m3/s",
            dynamicWater=False,
            backwaterSolved=False,
            metrics=water["metrics"],
        ),
        environment=dict(presets={"island": climate_metadata}),
        erosion=erosion,
        physicalScale=scale_contract(cell),
        metrics=dict(
            peakMetres=float(h.max()),
            landAreaMetres2=float(land.area),
            planningRegions=len(planning["planningRegions"]),
            biomeAreaMetres2={
                name: float(a.sum() * cell**2)
                for name, a in zip(BIOMES, arrays["biome_weights"])
            },
            seconds=time.perf_counter() - start,
        ),
        meshing=False,
        socialSolversRun=False,
        acceptance=dict(geographicMapReady=False, settlementReady=False),
        pending=[
            "multi-seed complete-island qualification",
            "resolution-independent erosion calibration",
            "dynamic lake and channel backwater",
            "regional dunes and finer beach/channel terrain",
            "roads, settlements and farmland",
        ],
    )
    product["erosion"][
        "qualification"
    ] = "regional preview at declared cell size; not invariant to resolution or calibrated geological time"
    product["physicalScale"][
        "recipe"
    ] = "48 km rugged tropical island; explicit 3.4-7.5 km uplift scales and ~2.7 km relief"
    product["checks"] = dict(
        finite=bool(np.isfinite(final).all()),
        allSourcesHighGround=bool(
            all(s["elevationMetres"] >= 350 for s in water["sources"])
        ),
        runoffConserved=bool(abs(water["metrics"]["massResidualCumecs"]) < 1e-8),
        lakesConserved=bool(
            all(abs(l["balanceResidualCumecs"]) < 1e-8 for l in water["lakes"])
        ),
        hasRivers=bool(water["rivers"]),
        hasConnectedLakes=bool(water["metrics"]["lakesWithVisibleRivers"]),
        hasDesert=bool(product["metrics"]["biomeAreaMetres2"]["desert"] > 1e6),
        hasRainforest=bool(product["metrics"]["biomeAreaMetres2"]["rainforest"] > 1e6),
        climateConserved=bool(
            all(
                abs(v["residual"]) / v["input"] < 1e-10
                for v in weather["monthlyBudgets"]
            )
        ),
    )
    if not all(product["checks"].values()):
        raise ValueError("geographic island gate failed " + str(product["checks"]))
    product["acceptance"]["geographicMapReady"] = True
    for name, a in dict(
        heightmap_metres=final,
        mountain_weight=w,
        coast_distance=coast,
        rock=rock,
        sand=sand,
        snow=snow,
        waterMask=water_mask,
        **drainage,
    ).items():
        np.save(root / (name.replace("_", "-") + ".npy"), a)
    folder = root / "island"
    folder.mkdir(exist_ok=True)
    for name, a in arrays.items():
        np.save(folder / (name.replace("_", "-") + ".npy"), a)
    np.save(folder / "heightmap-metres.npy", final)
    (root / "terrain-world.json").write_text(json.dumps(product, indent=2) + "\n")
    (root / "planning-map.json").write_text(json.dumps(planning, indent=2) + "\n")
    print(
        "PASS complete 2D geographic island: " + json.dumps(product["metrics"]),
        flush=True,
    )
    return product


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("output")
    p.add_argument("--seed", type=int, default=27)
    p.add_argument("--side", type=int, default=1025)
    a = p.parse_args()
    generate(a.output, a.seed, a.side)
