"""Compose current polygon maps with existing erosion, wind and drainage solvers."""

import argparse, json, time, copy
from pathlib import Path
import numpy as np
import shapely as sh
from shapely.geometry import shape, Point, LineString, mapping, GeometryCollection
from scipy.spatial import cKDTree
from generate_polygon_elevation import generate as generate_elevation
from terrain_landforms import erode_slopes, smooth, sample
from terrain_map_climate import simulate_map_climate
from terrain_island_drainage import build_drainage
from terrain_noise_mountains import gradient_noise
from terrain_geography import BIOMES
from terrain_map_surface_detail import reconstruct_relief, summit_snow
from terrain_dendritic_relief import biome_detail, incise_valleys
from terrain_massifs_canyons import compose_massifs, carve_canyons, breach_outlets


def seed_lake_basins(height, water, coast, cell, seed, budget=5):
    """Plan a small budget of valley basins ON existing river corridors.

    Excavation is explicit, not erosion. Priority-Flood subsequently decides the
    actual retained shoreline, spill level, outlet and storage of each basin.
    """
    dz, dx = np.gradient(height, cell)
    slope = np.hypot(dx, dz)
    candidates = []
    n = len(height)
    for r in water["rivers"]:
        ids = r["routingCells"]
        for k in range(2, len(ids) - 2):
            i = ids[k]
            z, x = divmod(i, n)
            if 40 < height[z, x] < 300 and slope[z, x] < 0.035 and coast[z, x] > 2200:
                candidates.append((r["discharge"], i, ids[k - 1], ids[k + 1]))
    result = height.copy()
    plans = []
    xx, zz = np.meshgrid(np.arange(n) * cell, np.arange(n) * cell)
    for q, i, up, down in sorted(candidates, reverse=True):
        c = np.array([i % n, i // n]) * cell
        if any(np.linalg.norm(c - p["centre"]) < 4500 for p in plans):
            continue
        direction = np.array([down % n - up % n, down // n - up // n], float)
        direction /= np.linalg.norm(direction)
        u = (xx - c[0]) * direction[0] + (zz - c[1]) * direction[1]
        v = -(xx - c[0]) * direction[1] + (zz - c[1]) * direction[0]
        radius = np.sqrt((u / 700) ** 2 + (v / 420) ** 2)
        radius /= 1 + 0.08 * gradient_noise(xx, zz, 650, seed + 800 + len(plans))
        cut = 24 * smooth(1 - radius)
        result -= cut
        plans.append(
            dict(
                id=len(plans) + 1,
                centre=c.tolist(),
                maximumCutMetres=24,
                majorRadiusMetres=700,
                minorRadiusMetres=420,
                excavatedCubicMetres=float(cut.sum() * cell**2),
                purpose="authored valley basin on an already routed river; final shoreline determined by spill solve",
            )
        )
        if len(plans) >= budget:
            break
    return result, plans


def generate(output, seed=27, source=None):
    root = Path(output)
    root.mkdir(parents=True, exist_ok=True)
    start = time.perf_counter()
    source = Path(source) if source else root / "elevation-input"
    if not (source / "elevation-map.json").exists():
        generate_elevation(source, seed)
    p = json.loads((source / "elevation-map.json").read_text())
    if p["seed"] != seed:
        raise ValueError("input seed mismatch")
    initial = np.load(source / "elevation-metres.npy")
    extent = p["extent"]
    cell = extent / (len(initial) - 1)
    axis = np.linspace(0, extent, len(initial))
    x, z = np.meshgrid(axis, axis)
    land = shape(p["land"])
    mask = sh.contains_xy(land, x, z)
    coast = sh.distance(
        sh.points(np.column_stack([x.ravel(), z.ravel()])), land.boundary
    ).reshape(x.shape)
    design = initial.copy()
    protected = np.zeros_like(mask)
    for feature in p["landforms"]:
        if feature["type"] == "plateau":
            protected |= sh.contains_xy(shape(feature["geometry"]), x, z)
    initial, reconstruction = reconstruct_relief(initial, coast, cell, seed, protected)
    np.save(root / "terrain-design.npy", design)
    initial, massifs = compose_massifs(
        initial, p["landforms"], coast, cell, seed, protected
    )
    np.save(root / "massif-elevation.npy", initial)
    provisional, _ = simulate_map_climate(p, initial, cell, mask)
    initial, biome_detail_report = biome_detail(
        initial, provisional["biome_weights"], coast, cell, seed, protected
    )
    np.save(root / "terrain-before-incision.npy", initial)
    initial, incision = incise_valleys(
        initial, mask, coast, cell, provisional["precipitation"], protected
    )
    np.save(root / "terrain-after-incision.npy", initial)
    initial, canyons = carve_canyons(
        initial, mask, coast, cell, provisional["precipitation"], seed, protected
    )
    np.save(root / "terrain-after-canyons.npy", initial)
    initial, breaching = breach_outlets(initial, mask, protected, cell)
    np.save(root / "terrain-after-breaching.npy", initial)
    print("PASS drainage-driven incision: " + json.dumps(incision), flush=True)
    dz, dx = np.gradient(initial, cell)
    erodibility = (
        (0.20 + 0.65 * smooth(np.hypot(dx, dz) / 0.20)) * mask * smooth(coast / 350)
    )
    for feature in p["landforms"]:
        if feature["type"] == "plateau":
            erodibility[sh.contains_xy(shape(feature["geometry"]), x, z)] = 0
    unprotected = np.full_like(initial, 1e9)
    h, erosion = erode_slopes(
        initial,
        cell,
        erodibility,
        unprotected,
        unprotected,
        mask,
        seed + 2400,
        "build/tests/WorldTerrainErosionRegional",
        crest_protection=0,
        erosion_settings=dict(
            dropletsPerCell=0.5,
            maximumSteps=45,
            maximumCutMetres=6,
            maximumDepositMetres=2,
        ),
    )
    np.save(root / "terrain-before-erosion.npy", initial)
    np.save(root / "eroded-elevation.npy", h)
    print("PASS bounded erosion: " + json.dumps(erosion), flush=True)
    climate, weather = simulate_map_climate(p, h, cell, mask)
    print(
        "PASS regional wind and 12 monthly climate states: "
        + json.dumps(weather["annualRainfallRangeMm"]),
        flush=True,
    )
    controls = dict(
        route_style="distributed",
        source_height=350,
        max_sources=42,
        max_lakes=5,
        source_spacing=1600,
        minimum_source_area=0.06e6,
        source_selection="high-ground",
        minimum_lake_area=60000,
    )
    preliminary, _ = build_drainage(
        h, mask, cell, land, climate["runoff_mm"], **(controls | dict(max_lakes=0))
    )
    basins, plans = seed_lake_basins(h, preliminary, coast, cell, seed)
    np.save(root / "basin-terrain.npy", basins)
    water, drainage = build_drainage(
        basins, mask, cell, land, climate["runoff_mm"], **controls
    )
    # Only lakes connected to the displayed river graph are retained as lakes.
    rejected = [l for l in water["lakes"] if not l["connectedRiverIds"]]
    for lake in rejected:
        drainage["lake_id"][drainage["lake_id"] == lake["id"]] = 0
    water["lakes"] = [l for l in water["lakes"] if l["connectedRiverIds"]]
    final = np.where(
        mask & (drainage["lake_id"] == 0), drainage["filled_level"], basins
    )
    drainage["conditioned_height"] = final
    water["metrics"]["lakes"] = len(water["lakes"])
    water["metrics"]["lakesWithVisibleRivers"] = len(water["lakes"])
    water["metrics"]["conditioningFillCubicMetres"] = float(
        ((final - basins) * mask).sum() * cell**2
    )
    from terrain_map_water_profiles import realize_profiles

    realize_profiles(water, land)
    # Climate labels belong to high-level regions; boundaries retain shared noise.
    lakegeom = sh.union_all([shape(l["geometry"]) for l in water["lakes"]])
    rivergeom = sh.union_all([shape(r["waterGeometry"]) for r in water["rivers"]])
    watergeom = sh.union_all([lakegeom, rivergeom]).intersection(land)
    # Fine land cover overrides coarse climate regions near flowing water.
    # This is a riparian opportunity corridor, not a simulated flood boundary.
    riparian = sh.union_all(
        [
            LineString(r["points"]).buffer(min(230, 75 + 100 * np.sqrt(r["discharge"])))
            for r in water["rivers"]
        ]
        + [shape(l["geometry"]).buffer(90) for l in water["lakes"]]
    )
    riparian = riparian.intersection(land).difference(watergeom)
    biomes = []
    for name in BIOMES:
        geom = (
            sh.union_all(
                [shape(r["geometry"]) for r in p["regions"] if r["biome"] == name]
            )
            .difference(watergeom)
            .difference(riparian)
        )
        biomes.append(
            dict(id=name, geometry=mapping(geom), areaMetres2=float(geom.area))
        )
    biomes.append(
        dict(
            id="riparian", geometry=mapping(riparian), areaMetres2=float(riparian.area)
        )
    )
    biomes.append(
        dict(
            id="freshwater",
            geometry=mapping(watergeom),
            areaMetres2=float(watergeom.area),
        )
    )
    np.save(root / "heightmap-metres.npy", final)
    np.save(root / "coast-distance.npy", coast)
    for name, a in drainage.items():
        np.save(root / (name.replace("_", "-") + ".npy"), a)
    folder = root / "climate"
    folder.mkdir(exist_ok=True)
    for name, a in climate.items():
        np.save(folder / (name.replace("_", "-") + ".npy"), a)
    # State the authority change explicitly instead of keeping stale corner paths.
    for c in p["corners"]:
        c["designElevationMetres"] = c["elevationMetres"]
        c["designLowerNeighbour"] = c.pop("lowerNeighbour")
        c["elevationMetres"] = (
            0.0 if c["coast"] else float(sample(final, [c["point"]], cell)[0])
        )
    for r in p["regions"]:
        r["designElevationMetres"] = r["elevationMetres"]
        r["elevationMetres"] = float(sample(final, [r["site"]], cell)[0])
    p["designPreview"] = p.pop("preview", None)
    p["designConditioning"] = p.pop("conditioning", None)
    p["preview"] = dict(
        cellMetres=cell,
        side=len(final),
        rowOrder="south-to-north",
        interpolation="post-erosion scalar height; vector regions own map areas, receiver/rank own drainage",
    )
    for r in p["regions"]:
        r["designTerrainClass"] = r["terrainClass"]
        value = r["elevationMetres"]
        r["terrainClass"] = (
            "lowland" if value < 144 else "hill" if value < 480 else "mountain"
        )
    p["terrainClassContours"] = {
        name: mapping(
            sh.union_all(
                [
                    shape(r["geometry"])
                    for r in p["regions"]
                    if r["terrainClass"] == name
                ]
            )
        )
        for name in ["lowland", "hill", "mountain"]
    }
    p["metrics"]["terrainAreaMetres2"] = {
        name: sum(r["areaMetres2"] for r in p["regions"] if r["terrainClass"] == name)
        for name in ["lowland", "hill", "mountain"]
    }
    snow = summit_snow(final, mask, cell, seed)
    np.save(root / "snow-cover.npy", snow)
    p.update(
        stage="integrated-map-geography-v34",
        reliefReconstruction=reconstruction,
        biomeSurfaceDetail=biome_detail_report,
        valleyIncision=incision,
        mountainMassifs=massifs,
        canyonCarving=canyons,
        valleyBreaching=breaching,
        snow=dict(
            model="art-directed altitude cover, not climate snowfall or runoff storage",
            snowlineMetres=1120,
            transitionMetres=210,
            raster="snow-cover.npy",
            areaWeightedMetres2=float(snow.sum() * cell**2),
        ),
        rivers=water["rivers"],
        lakes=water["lakes"],
        sources=water["sources"],
        riverNodes=water["nodes"],
        erosion=erosion,
        lakePlanning=plans,
        riparian=dict(
            model="flow-scaled river and lake-bank land-cover override; not flood simulation",
            areaMetres2=float(riparian.area),
        ),
        biomes=biomes,
        climate=weather,
        surface=dict(
            type="2D scalar terrain and vector geography; no mesh",
            side=len(final),
            cellMetres=cell,
            raster="heightmap-metres.npy",
            authority="post-erosion scalar elevation, Priority-Flood receiver/rank for drainage; original design corner graph retained separately",
        ),
        hydrology=dict(
            controls=controls,
            metrics=water["metrics"],
            dynamicWater=False,
            backwaterSolved=False,
            runoff="smooth annual proxy P * exp(-PET/P); complementary evapotranspiration, steady mean m3/s",
            lakePlacement="budgeted authored valley excavation, then terrain spill and storage solve",
        ),
        pipeline=dict(
            elevationReady=True,
            erosion="two bounded particle passes",
            wind="regional conservative potential flow",
            climate="12 advective monthly states",
            rivers="terrain-derived source-to-ocean network",
            lakes="connected retained basins with spill outlets",
            biomes="climate-labelled shared polygon regions",
            settlements="deferred",
            meshing=False,
        ),
    )
    p["metrics"].update(
        peakMetres=float(final.max()),
        biomeAreaMetres2={b["id"]: b["areaMetres2"] for b in biomes},
        seconds=time.perf_counter() - start,
    )
    p["checks"] = dict(
        finite=bool(np.isfinite(final).all()),
        hasHighGroundSources=bool(water["sources"])
        and all(s["elevationMetres"] >= 350 for s in water["sources"]),
        hasConnectedLakes=bool(water["lakes"]),
        massConserved=abs(water["metrics"]["massResidualCumecs"]) < 1e-8,
        hasDesert=any(b["id"] == "desert" and b["areaMetres2"] > 1e6 for b in biomes),
        hasRainforest=any(
            b["id"] == "rainforest" and b["areaMetres2"] > 1e6 for b in biomes
        ),
    )
    (root / "terrain-world.json").write_text(json.dumps(p, indent=2) + "\n")
    print("CHECK integrated geography: " + json.dumps(p["checks"]), flush=True)
    print("PASS drainage solve: " + json.dumps(water["metrics"]), flush=True)
    if not all(p["checks"].values()):
        raise ValueError("integrated geography check failed")
    return p


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("output")
    p.add_argument("--source")
    p.add_argument("--seed", type=int, default=27)
    a = p.parse_args()
    generate(a.output, a.seed, a.source)
