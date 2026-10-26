"""Generate v9 canonical 2D landscape evidence; no social solvers or meshes."""

import argparse
import hashlib
import json
import time
import subprocess
from pathlib import Path

import numpy as np
import shapely as sh
from shapely.geometry import LineString, Point, mapping, shape
from terrain_water_realization import read_source
from terrain_landforms import (
    canonical_coast,
    sharpen_watersheds,
    erode_slopes,
    spline_cliffs,
    river_network,
    connected_lakes,
    LandscapeSurface,
    cover_fields,
    cover_polygon,
    sample,
    ridge_sections,
)


def rebuild_source(directory, foundation_binary, erosion_binary):
    """Rebuild the actual C++ paper graph/crests before any display refinement."""
    from build_hydrology_terrain import build

    root = Path(directory)
    root.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            str(Path(foundation_binary).resolve()),
            str(root / "foundation.json"),
            "2048",
            "--mountain-range",
        ],
        check=True,
    )
    build(json.loads((root / "foundation.json").read_text()), root, erosion_binary)
    manifest = {
        p.name: hashlib.sha256(p.read_bytes()).hexdigest()
        for p in root.iterdir()
        if p.is_file() and p.name != "manifest.json"
    }
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def generate(source, output, binary, side=1025):
    start = time.perf_counter()
    source, output = Path(source), Path(output)
    old, original, _ = read_source(source)
    f = json.loads((source / "foundation.json").read_text())
    weight = np.load(source / "mountain-weight.npy")
    output.mkdir(parents=True, exist_ok=True)
    land = canonical_coast(shape(old["land"]))
    cell = f["extent"] / (side - 1)
    print(
        f"PASS canonical coast: {land.area/1e6:.3f} km2; valid, same topology",
        flush=True,
    )
    h, w, dr, dc, coast, mask = sharpen_watersheds(
        f, original, weight, old["rivers"], land, side
    )
    before = h.copy()
    h, erosion = erode_slopes(h, cell, w, dr, dc, mask, f["seed"], binary)
    h, cliffs = spline_cliffs(h, cell, w, dr)
    rivers, heads = river_network(old["rivers"], h, w, cell, land)
    lakes = connected_lakes(rivers, land, seed=f["seed"])
    surface = LandscapeSurface(h, cell, land, rivers, lakes)
    detailed = surface.raster(side)
    for river in rivers:
        # Lake basins can deepen below the construction channel profile and
        # rise towards their outlet. Publish the realized bed for later consumers.
        bed = surface.sample(river["points"])
        river["realizedBedMetres"] = bed.tolist()
        river["waterDepthMetres"] = np.maximum(
            0, np.array(river["waterSurfaceMetres"]) - bed
        ).tolist()
    for lake in lakes:
        p = shape(lake["geometry"])
        x0, z0, x1, z1 = p.bounds
        step = 1.5
        x, z = np.meshgrid(
            np.arange(x0 + step / 2, x1, step), np.arange(z0 + step / 2, z1, step)
        )
        q = np.column_stack([x.ravel(), z.ravel()])
        inside = sh.covers(p, sh.points(q))
        depths = lake["waterLevelMetres"] - surface.sample(q[inside])
        lake.update(
            storageCubicMetres=float(depths.sum() * step**2),
            maximumDepthMetres=float(depths.max()),
            storageIntegrationStepMetres=step,
            storageModel="sampled continuous bed, midpoint integration",
        )
        ports = [lake["inletPosition"], lake["outletPosition"]]
        lake["portBedMetres"] = surface.sample(ports).tolist()
        shore = sh.get_coordinates(p.boundary)
        line = LineString(
            next(r for r in rivers if r["from"] == lake["riverFrom"])["points"]
        )
        valid = sh.distance(sh.points(shore), line) > lake["shorePortExclusionMetres"]
        lake["maximumShoreErrorMetres"] = float(
            np.abs(surface.sample(shore[valid]) - lake["waterLevelMetres"]).max()
        )
        if lake["maximumShoreErrorMetres"] > 1e-6:
            raise ValueError("lake shoreline mismatch")
        if not all(b < lake["waterLevelMetres"] for b in lake["portBedMetres"]):
            raise ValueError("dry lake port")
    covers = cover_fields(detailed, cell, land, coast, cliffs, rivers, lakes)
    summer = cover_fields(
        detailed, cell, land, coast, cliffs, rivers, lakes, season="summer"
    )
    regions = {
        key: mapping(cover_polygon(covers[key], cell, land))
        for key in ["sand", "snow", "rock"]
    }
    source_info = [
        dict(
            node=i,
            position=next(r["points"][0] for r in rivers if r["from"] == i),
            heightMetres=float(
                surface.sample(
                    [next(r["points"][0] for r in rivers if r["from"] == i)]
                )[0]
            ),
        )
        for i in heads
    ]
    lookup = {r["from"]: r for r in rivers}
    routes = []
    for head in heads:
        ids = []
        i = head
        while i in lookup:
            if i in ids:
                raise ValueError("river cycle")
            ids.append(i)
            i = lookup[i]["to"]
        last = lookup[ids[-1]]
        if last["terminal"] != "ocean" or land.covers(Point(last["points"][-1])):
            raise ValueError("inland river terminal")
        routes.append(dict(source=head, reaches=ids, mouth=last["to"]))
    terminal_q = sum(r["discharge"] for r in rivers if r["terminal"] == "ocean")
    input_q = sum(r["localRunoffCumecs"] for r in rivers)
    dz, dx = np.gradient(detailed, cell)
    oldz, oldx = np.gradient(original, f["extent"] / (len(original) - 1))
    sections = ridge_sections(f, original, detailed, w, cell)
    total_runoff = sum(row["local"] - row["loss"] for row in f["ledger"])
    metrics = dict(
        peakMetres=float(detailed.max()),
        ridgeSections=len(sections),
        oldMedianRidgeProminenceMetres=float(
            np.median([r["beforeProminenceMetres"] for r in sections])
        ),
        newMedianRidgeProminenceMetres=float(
            np.median([r["afterProminenceMetres"] for r in sections])
        ),
        mountainLandFraction=float(((w > 0.5) & mask).sum() / mask.sum()),
        highGroundSources=len(heads),
        minimumSourceHeightMetres=min(s["heightMetres"] for s in source_info),
        mappedReaches=len(rivers),
        oceanMouths=sum(r["terminal"] == "ocean" for r in rivers),
        connectedLakes=len(lakes),
        riverBalanceResidualCumecs=terminal_q - input_q,
        oceanDischargeCumecs=terminal_q,
        sandAreaMetres2=shape(regions["sand"]).area,
        snowAreaMetres2=shape(regions["snow"]).area,
        rockAreaMetres2=shape(regions["rock"]).area,
        cliffs=len(cliffs),
        maximumCliffDropMetres=max(
            max(np.array(c["crestHeightMetres"]) - c["toeHeightMetres"]) for c in cliffs
        ),
        oldSlopeP95=float(np.percentile(np.hypot(oldx, oldz)[weight > 0.5], 95)),
        newSlopeP95=float(np.percentile(np.hypot(dx, dz)[w > 0.5], 95)),
        seconds=time.perf_counter() - start,
    )
    checks = dict(
        highGroundSources=metrics["minimumSourceHeightMetres"] >= 70,
        allSourceChainsReachOcean=len(routes) == len(heads),
        riverFlowConserved=abs(terminal_q - input_q) < 1e-8,
        connectedWetLakePorts=all(
            min(l["portBedMetres"]) < l["waterLevelMetres"] for l in lakes
        ),
        lakeSteadyFlowConserved=all(
            l["inletDischargeCumecs"] == l["outletDischargeCumecs"] for l in lakes
        ),
        actualCliffRelief=metrics["maximumCliffDropMetres"] > 15,
        sandyCoast=metrics["sandAreaMetres2"] > 10000,
        seasonalSnow=metrics["snowAreaMetres2"] > 10000 and summer["snow"].max() == 0,
        validCoverContours=all(shape(v).is_valid for v in regions.values()),
        finiteHeight=bool(np.isfinite(detailed).all()),
    )
    checks = {key: bool(value) for key, value in checks.items()}
    if not all(checks.values()):
        raise ValueError(f"landform acceptance failed: {checks}")
    product = dict(
        schema=1,
        stage="terrain-landforms-v9",
        terrainMethod=old["terrainMethod"],
        mountainRangeGuide=f.get("mountainRangeGuide", []),
        mountainRangeControls=f.get("mountainRangeControls", {}),
        paperImplementation=dict(
            reference="Genevaux et al. 2013, sections 4–7",
            watershedCrests=len(f["crestVertices"]),
            terrainPrimitives=len(f["terrainPrimitives"]),
            profileExponent=1.65,
            ridgeReliefGain=1.2,
            compactRefinementRadiusMetres=38,
            noiseWavelengthsMetres=[160, 65, 28],
            erosionIsSeparateExtension=True,
        ),
        seed=f["seed"],
        extent=f["extent"],
        source=str(source),
        sourceTerrainSha256=hashlib.sha256(
            (source / "terrain-map.json").read_bytes()
        ).hexdigest(),
        land=mapping(land),
        coast=dict(
            method="periodic arclength Gaussian, sigma 36 m",
            sampleSpacingMetres=6,
            areaChangeFraction=land.area / shape(old["land"]).area - 1,
        ),
        ridgeSections=sections,
        waterBudget=dict(
            totalRunoffCumecs=total_runoff,
            mappedRiverOutletsCumecs=terminal_q,
            otherCoastalDrainageCumecs=total_runoff - terminal_q,
            sourceLedger="source/foundation.json#ledger",
            otherDrainageDisplay="lowland construction drainage is not mapped as highland rivers",
        ),
        rivers=rivers,
        lakes=lakes,
        cliffs=cliffs,
        sources=source_info,
        sourceToOceanRoutes=routes,
        covers=regions,
        climate=dict(
            preset="maritime-winter",
            seaLevelTemperatureC=0.65,
            lapseCPerMetre=0.0065,
            snowType="seasonal cover; not permanent alpine snow",
            summerSeaLevelTemperatureC=16.0,
        ),
        surface=dict(
            type="watershed-height plus continuous river/lake primitives",
            baseHeight="base-height.npy",
            raster="heightmap-metres.npy",
            cellMetres=cell,
            side=side,
            rasterIsApproximation=True,
            riverBedProperty="realizedBedMetres; bed is the construction profile",
            rowOrder="south-to-north",
        ),
        erosion=erosion,
        metrics=metrics,
        checks=checks,
        meshing=False,
        socialSolversRun=[],
        acceptance=dict(
            terrainOnlyChecksPassed=True,
            settlementReady=False,
            dynamicBackwaterSolved=False,
        ),
        pending=[
            "Multi-seed landscape qualification and social-map integration",
            "Dynamic flood/backwater simulation; present lakes use steady design stages",
        ],
    )
    arrays = dict(
        base_height=h,
        heightmap_metres=detailed,
        source_height=original,
        mountain_weight=w,
        terrain_before_erosion=before,
        land_mask=mask,
        coast_distance=coast,
    )
    arrays.update(covers)
    for name, a in arrays.items():
        np.save(output / (name.replace("_", "-") + ".npy"), a)
    (output / "terrain-landforms.json").write_text(json.dumps(product, indent=2) + "\n")
    print("PASS terrain landforms: " + json.dumps(metrics), flush=True)
    print("PASS acceptance: " + json.dumps(checks), flush=True)
    return product


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("source")
    p.add_argument("output")
    p.add_argument("--erosion-binary", default="build/tests/WorldTerrainErosion")
    p.add_argument("--side", type=int, default=1025)
    p.add_argument("--rebuild-source", action="store_true")
    p.add_argument("--foundation-binary", default="build/tests/WorldIslandFoundation")
    a = p.parse_args()
    if a.rebuild_source:
        rebuild_source(a.source, a.foundation_binary, a.erosion_binary)
    generate(a.source, a.output, a.erosion_binary, a.side)
