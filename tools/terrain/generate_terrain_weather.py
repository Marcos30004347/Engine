"""Sharper watershed divides, wider channels, and terrain-driven monthly climate."""

import argparse, copy, hashlib, json, time
from pathlib import Path
import numpy as np
import shapely as sh
from shapely.geometry import shape, LineString, mapping
from terrain_landforms import (
    sharpen_watersheds,
    erode_slopes,
    LandscapeSurface,
    cover_fields,
    cover_polygon,
    ridge_sections,
    sample,
    smooth,
)
from terrain_geography import regional_graph, geographic_regions, biome_partition
from terrain_regional_weather import raster_climate, RECIPES
from generate_terrain_geography import read_baseline, write_source_manifest


def widen_rivers(rivers, lakes, factor=2.0):
    rivers = copy.deepcopy(rivers)
    lakes = copy.deepcopy(lakes)
    depths = {}
    for r in rivers:
        r["width"] *= factor
        r["depth"] *= 1.2
        r["velocity"] = r["discharge"] / ((2 / 3) * r["width"] * r["depth"])
        for node in (r["from"], r["to"]):
            depths[node] = max(depths.get(node, 0), r["depth"])
    for r in rivers:
        p = np.array(r["points"])
        arc = np.r_[0, np.cumsum(np.linalg.norm(np.diff(p, axis=0), axis=1))]
        u = arc / arc[-1]
        r["bed"] = (
            np.array(r["waterSurfaceMetres"])
            - ((1 - u) * depths[r["from"]] + u * depths[r["to"]])
        ).tolist()
    for lake in lakes:
        r = next(r for r in rivers if r["from"] == lake["riverFrom"])
        lake["shorePortExclusionMetres"] = max(12, r["width"] * 2)
    return rivers, lakes


def generate(source, output, binary, mountain_model="watershed", cliffs_enabled=False):
    if mountain_model not in ("watershed", "noise", "regional"):
        raise ValueError("unknown mountain model")
    started = time.perf_counter()
    source = Path(source)
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    j = json.loads((source / "terrain-geography.json").read_text())
    manifest = json.loads((source / "manifest.json").read_text())
    hashes = {}
    for name in [
        "terrain-geography.json",
        "base-height.npy",
        "heightmap-metres.npy",
        "coast-distance.npy",
        "mountain-weight.npy",
    ]:
        value = hashlib.sha256((source / name).read_bytes()).hexdigest()
        if manifest.get(name) != value:
            raise ValueError("v10 checksum mismatch: " + name)
        hashes[name] = value
    v9 = Path(j["source"])
    old9, ancestor_hashes = read_baseline(v9)
    f = json.loads((v9 / "source/foundation.json").read_text())
    terrain = json.loads((v9 / "source/terrain-map.json").read_text())
    oldheight = np.load(v9 / "source/heightmap-metres.npy")
    oldweight = np.load(v9 / "source/mountain-weight.npy")
    before = np.load(v9 / "terrain-before-erosion.npy")
    base = np.load(source / "base-height.npy")
    original = np.load(source / "heightmap-metres.npy")
    cell = j["surface"]["cellMetres"]
    land = shape(j["land"])
    side = len(base)
    if not cliffs_enabled:
        from terrain_geography import coastal_bluffs

        coast = np.load(source / "coast-distance.npy")
        uplift, _ = coastal_bluffs(
            np.zeros_like(base), cell, land, j["rivers"], j["lakes"], coast
        )
        base -= uplift
        j["geography"]["features"] = [
            feature
            for feature in j["geography"]["features"]
            if "cliff" not in feature["type"]
        ]
        j["geography"]["existingInlandCliffs"] = []
        j["cliffs"] = []
        print(
            "PASS explicit cliffs disabled: coastal uplift removed before regional generation",
            flush=True,
        )
    rivers, lakes = widen_rivers(j["rivers"], j["lakes"])
    if mountain_model in ("noise", "regional"):
        from terrain_noise_mountains import noise_mountains

        w = np.load(source / "mountain-weight.npy")
        coast = np.load(source / "coast-distance.npy")
        x, z = np.meshgrid(np.arange(side) * cell, np.arange(side) * cell)
        mask = sh.contains_xy(land, x, z)
        if mountain_model == "regional":
            straight = sharpen_watersheds(
                f, oldheight, oldweight, terrain["rivers"], land, side, angular=True
            )[0]
            curved = sharpen_watersheds(
                f,
                oldheight,
                oldweight,
                terrain["rivers"],
                land,
                side,
                angular=True,
                crest_noise=True,
            )[0]
            base += (curved - straight) * smooth(w)
            np.save(output / "watershed-straight.npy", straight)
            np.save(output / "watershed-noisy-crests.npy", curved)
            print(
                "PASS watershed crest lines: shared lateral bends and height notches",
                flush=True,
            )
        base, dr, mountain_model_meta = noise_mountains(
            base, cell, w, coast, land, rivers, f["seed"], f["mountainRangeGuide"]
        )
        dc = np.full_like(base, 1e6)  # No old divide is exempt from erosion.
        j["cliffs"] = (
            []
        )  # These artificial inland scarps belonged to the replaced surface.
        # Reapply recorded compact outcrops on the replacement surface.
        for feature in j["geography"]["features"]:
            if feature["type"] != "rock-outcrop":
                continue
            c = np.mean(feature["crest"], axis=0)
            tangent = np.array(feature["orientation"])
            normal = np.array([-tangent[1], tangent[0]])
            along = (x - c[0]) * tangent[0] + (z - c[1]) * tangent[1]
            across = (x - c[0]) * normal[0] + (z - c[1]) * normal[1]
            bump = (
                feature["heightAddedMetres"]
                * (1 - smooth(abs(along) / 65))
                * (1 - smooth(abs(across) / 24))
            )
            base += bump * smooth(w)
    else:
        sharp, w, dr, dc, coast, mask = sharpen_watersheds(
            f, oldheight, oldweight, terrain["rivers"], land, side, angular=True
        )
        base = base + (sharp - before) * smooth(w)
    erosion_weight = w
    if mountain_model == "regional":
        from terrain_regional_detail import (
            expand_ranges,
            regional_weathering,
            desert_detail,
            geology_clearance,
        )

        base, w, ranges, dr = expand_ranges(
            base, cell, w, coast, land, rivers, j["geography"]["features"], f, f["seed"]
        )
        base, erosion_weight, erosion_classes = regional_weathering(
            base,
            cell,
            w,
            coast,
            land,
            rivers,
            lakes,
            j["geography"]["features"],
            f["seed"],
        )
        print(
            "PASS range planning: " + str(len(ranges)) + " added ranges/massifs",
            flush=True,
        )
    pre = base.copy()
    base, erosion = erode_slopes(
        base,
        cell,
        erosion_weight,
        dr,
        dc,
        mask,
        f["seed"] + 200,
        binary,
        crest_protection=0 if mountain_model in ("noise", "regional") else 4,
    )
    print(
        "PASS mountain construction: "
        + mountain_model
        + "; two subsequent erosion passes",
        flush=True,
    )
    if mountain_model == "regional":
        delta = pre - base
        erosion["byLandform"] = {}
        for kind, eligible in erosion_classes.items():
            erosion["byLandform"][kind] = dict(
                changedAreaMetres2=float(
                    np.count_nonzero((abs(delta) > 0.01) & eligible) * cell**2
                ),
                maximumCutMetres=float(np.max(delta[eligible], initial=0)),
                maximumDepositMetres=float(np.max(-delta[eligible], initial=0)),
            )
        print(
            "PASS erosion by landform: " + json.dumps(erosion["byLandform"]), flush=True
        )
    surface = LandscapeSurface(base, cell, land, rivers, lakes)
    h = surface.raster(side)
    for r in rivers:
        r["realizedBedMetres"] = surface.sample(r["points"]).tolist()
        r["waterDepthMetres"] = (
            np.array(r["waterSurfaceMetres"]) - r["realizedBedMetres"]
        ).tolist()
    for lake in lakes:
        p = shape(lake["geometry"])
        x0, z0, x1, z1 = p.bounds
        step = 1.5
        x, z = np.meshgrid(
            np.arange(x0 + step / 2, x1, step), np.arange(z0 + step / 2, z1, step)
        )
        q = np.column_stack([x.ravel(), z.ravel()])
        depth = lake["waterLevelMetres"] - surface.sample(q[sh.covers(p, sh.points(q))])
        lake["storageCubicMetres"] = float(depth.sum() * step**2)
        lake["maximumDepthMetres"] = float(depth.max())
        lake["portBedMetres"] = surface.sample(
            [lake["inletPosition"], lake["outletPosition"]]
        ).tolist()
        shore = sh.get_coordinates(p.boundary)
        line = LineString(
            next(r for r in rivers if r["from"] == lake["riverFrom"])["points"]
        )
        valid = sh.distance(sh.points(shore), line) > lake["shorePortExclusionMetres"]
        lake["maximumShoreErrorMetres"] = float(
            np.abs(surface.sample(shore[valid]) - lake["waterLevelMetres"]).max()
        )
    features = j["geography"]["features"]
    for feature in features + j["cliffs"]:
        if "toe" in feature:
            feature["crestHeightMetres"] = surface.sample(feature["crest"]).tolist()
            feature["toeHeightMetres"] = surface.sample(feature["toe"]).tolist()
    fields, regions = geographic_regions(
        h, cell, land, w, rivers, lakes, features, j["cliffs"]
    )
    covers = cover_fields(
        h, cell, land, coast, j["cliffs"], rivers, lakes, season="summer"
    )
    covers["rock"] = np.maximum(covers["rock"], fields["scree"] * 0.4)
    graph = regional_graph(f, land, h, cell)
    water = sh.union_all(
        [shape(l["geometry"]) for l in lakes]
        + [LineString(r["points"]).buffer(r["width"] / 2) for r in rivers]
    )
    product = copy.deepcopy(j)
    product.update(
        stage="terrain-weather-v11",
        cliffsEnabled=cliffs_enabled,
        source=str(source),
        sourceHashes=hashes,
        ancestorSource=str(v9),
        ancestorSourceHashes=ancestor_hashes,
        rivers=rivers,
        lakes=lakes,
        climate=dict(
            preset="temperate",
            stylized=False,
            windSimulation="steady regional potential flow",
            monthlyTemperature=True,
        ),
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
        erosion=erosion,
        pending=[
            "Couple annual climate runoff to the existing design-discharge river scenario",
            "Dynamic flooding, weather turbulence and vertical convection",
            "Multi-seed visual qualification and social-map integration",
        ],
    )
    product["geography"]["regions"] = regions
    product["ridgeSections"] = ridge_sections(f, original, h, w, cell)
    product["paperImplementation"].update(
        profileExponent=1.35,
        ridgeReliefGain=1.15,
        crestProjection="shared canonical non-river-crossing edges",
        crestBlend=0.98,
        crossCrestConvolution=False,
        flankFilterTransitionMetres=55,
        noiseCrestProtectionMetres=14,
    )
    if mountain_model in ("noise", "regional"):
        product["stage"] = "terrain-noise-mountains-v13"
        product["terrainMethod"] = "range-guided-ridged-noise-hydraulic-erosion-v13"
        product["surface"][
            "type"
        ] = "Range-guided ridged noise, hydraulic erosion, retained spline geology and canonical water"
        product["mountainModel"] = mountain_model_meta
        product["paperImplementation"] = dict(
            reference=j["paperImplementation"]["reference"],
            role="Inherited drainage topology only; noise mountain surface is a separate experiment",
            watershedCrests=False,
            erosionIsSeparateExtension=True,
        )
        product["ridgeSections"] = []
        product["pending"].append(
            "Recompute drainage on the eroded noise field and regenerate inland cliff contours"
        )
    if mountain_model == "regional":
        product["stage"] = (
            "terrain-regional-relief-v14"
            if cliffs_enabled
            else "terrain-regional-relief-v15-no-cliffs"
        )
        product["terrainMethod"] = "multiple-ranges-regional-erosion-aeolian-noise-v14"
        product["mountainModel"]["additionalRanges"] = ranges
        product["mountainModel"]["inheritedWatershedLineNoise"] = dict(
            lateralWavelengthsMetres=[260, 95, 38],
            lateralAmplitudesMetres=[32, 14, 5],
            heightWavelengthsMetres=[180, 60],
            heightAmplitudesMetres=[12, 4],
            sharedJunctions=True,
            role="Correct inherited crests in the blend collar; noise interior remains primary",
        )
        product["surface"][
            "presetTerrain"
        ] = "Each preset/heightmap-metres.npy includes its climate-specific surface detail"
        product["desertDetail"] = {}
        np.save(output / "erosion-weight.npy", erosion_weight)
        for name, domain in erosion_classes.items():
            np.save(output / ("erosion-domain-" + name + ".npy"), domain)
    for preset in RECIPES:
        arrays, meta = raster_climate(graph, h, cell, mask, coast, preset)
        meta["biomes"] = biome_partition(arrays["biome_weights"], cell, land, water)
        meta["biomeAreaMetres2"] = {b["id"]: b["areaMetres2"] for b in meta["biomes"]}
        product["environment"]["presets"][preset] = meta
        folder = output / preset
        folder.mkdir(exist_ok=True)
        for key, a in arrays.items():
            np.save(
                folder / (key.replace("_", "-") + ".npy"),
                (
                    a.astype(np.float32)
                    if a.ndim == 3 and key == "monthly_temperature"
                    else a
                ),
            )
        if mountain_model == "regional":
            axis = np.arange(side) * cell
            grid_x, grid_z = np.meshgrid(axis, axis)
            q = np.column_stack([grid_x.ravel(), grid_z.ravel()])
            water_distance = sh.distance(sh.points(q), water).reshape(h.shape)
            clear = geology_clearance(j["geography"]["features"], q).reshape(h.shape)
            # Coherent prevailing vector prevents discontinuities from independently
            # rotating a dune phase at each climate cell.
            wind = np.mean(arrays["wind"][:, mask], axis=1)
            detail, dune_weight, detail_meta = desert_detail(
                h,
                cell,
                arrays["biome_weights"][0],
                wind,
                coast,
                water_distance,
                clear,
                f["seed"],
            )
            np.save(folder / "dune-height.npy", detail)
            np.save(folder / "dune-weight.npy", dune_weight)
            np.save(folder / "heightmap-metres.npy", h + detail)
            product["desertDetail"][preset] = detail_meta
            print(
                "PASS desert detail " + preset + ": " + json.dumps(detail_meta),
                flush=True,
            )
        print(
            "PASS regional wind/temperature "
            + preset
            + ": "
            + json.dumps(meta["temperatureRangeC"])
            + "; "
            + json.dumps(meta["biomeAreaMetres2"]),
            flush=True,
        )
    arrays = dict(
        base_height=base,
        heightmap_metres=h,
        mountain_weight=w,
        coast_distance=coast,
        terrain_before_erosion=pre,
        crest_distance=dc,
        river_distance=dr,
    )
    arrays.update(covers)
    arrays.update(fields)
    for key, a in arrays.items():
        np.save(output / (key.replace("_", "-") + ".npy"), a)
    product["covers"] = {
        key: mapping(cover_polygon(covers[key], cell, land))
        for key in ["sand", "rock", "snow"]
    }
    for s in product["sources"]:
        s["heightMetres"] = float(surface.sample([s["position"]])[0])
    sections = product["ridgeSections"]
    metrics = dict(
        peakMetres=float(h.max()),
        highGroundSources=len(product["sources"]),
        minimumSourceHeightMetres=min(s["heightMetres"] for s in product["sources"]),
        oceanMouths=sum(r["terminal"] == "ocean" for r in rivers),
        connectedLakes=len(lakes),
        medianOldRidgeProminenceMetres=float(
            np.median([s["beforeProminenceMetres"] for s in sections])
            if sections
            else 0
        ),
        medianNewRidgeProminenceMetres=float(
            np.median([s["afterProminenceMetres"] for s in sections]) if sections else 0
        ),
        minimumWidthMetres=min(r["width"] for r in rivers),
        maximumWidthMetres=max(r["width"] for r in rivers),
        widthScale=2.0,
        depthScale=1.2,
        seconds=time.perf_counter() - started,
    )
    if mountain_model in ("noise", "regional"):
        del metrics["medianOldRidgeProminenceMetres"]
        del metrics["medianNewRidgeProminenceMetres"]
        delta = pre - base
        metrics["erosionChangedAreaMetres2"] = float(
            np.count_nonzero(abs(delta) > 0.05) * cell**2
        )
        metrics["erosionMaximumCutMetres"] = float(delta.max())
        metrics["erosionMaximumDepositMetres"] = float(-delta.min())
    product["metrics"] = metrics
    product["checks"] = dict(
        finiteHeight=bool(np.isfinite(h).all()),
        highSources=metrics["minimumSourceHeightMetres"] >= 70,
        wetLakePorts=all(
            max(l["portBedMetres"]) < l["waterLevelMetres"] for l in lakes
        ),
        closedFlowBudget=abs(
            sum(r["localRunoffCumecs"] for r in rivers)
            - sum(r["discharge"] for r in rivers if r["terminal"] == "ocean")
        )
        < 1e-8,
        widerChannels=all(
            r["width"] >= o["width"] * 1.99 for r, o in zip(rivers, j["rivers"])
        ),
    )
    if not all(product["checks"].values()):
        raise ValueError("terrain weather gate failed " + str(product["checks"]))
    product["acceptance"].update(
        terrainOnlyChecksPassed=True,
        settlementReady=False,
        climateHydrologyCoupled=False,
    )
    (output / "terrain-weather.json").write_text(json.dumps(product, indent=2) + "\n")
    write_source_manifest(output, "terrain-weather.json")
    print("PASS terrain/weather: " + json.dumps(metrics), flush=True)
    return product


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("source")
    p.add_argument("output")
    p.add_argument("--erosion-binary", default="build/tests/WorldTerrainErosion")
    p.add_argument(
        "--mountain-model",
        choices=["watershed", "noise", "regional"],
        default="watershed",
    )
    p.add_argument(
        "--enable-cliffs",
        action="store_true",
        help="Restore dedicated coastal cliff uplift (disabled by default)",
    )
    a = p.parse_args()
    generate(a.source, a.output, a.erosion_binary, a.mountain_model, a.enable_cliffs)
