"""Design-first polygon elevation. No erosion, river, climate or mesh solve."""

import argparse, heapq, json, time
from pathlib import Path
import numpy as np
import shapely as sh
from shapely.geometry import Polygon, MultiPoint, box, Point, mapping
from scipy.interpolate import LinearNDInterpolator
from PIL import Image, ImageDraw
from terrain_noise_mountains import gradient_noise
from render_terrain_landforms import font


def coast(seed, extent):
    rng = np.random.default_rng(seed)
    angle = np.linspace(0, 2 * np.pi, 721)[:-1]
    phases = rng.uniform(0, 2 * np.pi, 5)
    radius = extent * (
        0.37
        + 0.035 * np.sin(3 * angle + phases[0])
        + 0.025 * np.sin(5 * angle + phases[1])
        + 0.013 * np.sin(9 * angle + phases[2])
        + 0.006 * np.sin(19 * angle + phases[3])
    )
    xy = np.column_stack(
        [
            extent * 0.5 + radius * np.cos(angle),
            extent * 0.5 + 1.09 * radius * np.sin(angle),
        ]
    )
    return Polygon(xy)


def build(
    seed=27,
    extent=48000.0,
    count=480,
    peak=1600.0,
    composition=False,
    archipelago_mode=False,
    land_style=None,
    rolling_relief=None,
):
    if extent <= 0 or count < 30 or peak <= 0:
        raise ValueError("invalid polygon map controls")
    if rolling_relief is None:
        rolling_relief = land_style == "natural"
    land = coast(seed, extent)
    coastline = None
    if land_style == "natural":
        from terrain_natural_coast import natural_coast

        land, coastline = natural_coast(seed, extent)
        archipelago_mode = True
    elif archipelago_mode:
        from terrain_archipelago import archipelago

        land, coastline = archipelago(seed, extent)
    if archipelago_mode:
        from terrain_archipelago import island_cells

        cells, sites, island_ids = island_cells(land, seed, count)
    else:
        rng = np.random.default_rng(np.random.SeedSequence([seed, 1900]))
        candidates = rng.uniform(0, extent, (count * 8, 2))
        sites = candidates[sh.contains_xy(land, candidates[:, 0], candidates[:, 1])][
            :count
        ]
        for _ in range(3):
            raw = sh.get_parts(
                sh.voronoi_polygons(
                    MultiPoint(sites), extend_to=box(0, 0, extent, extent), ordered=True
                )
            )
            cells = [p.intersection(land) for p in raw]
            # Two Lloyd passes; the third construction uses the relaxed sites.
            if _ < 2:
                sites = np.array([[p.centroid.x, p.centroid.y] for p in cells])
        island_ids = [0] * len(cells)
    corners = []
    lookup = {}
    regions = []
    edge_owners = {}

    def corner_id(p):
        key = tuple(np.round(p, 5))
        if key not in lookup:
            lookup[key] = len(corners)
            corners.append(list(map(float, p)))
        return lookup[key]

    for rid, (p, site) in enumerate(zip(cells, sites)):
        rings = []
        for part in sh.get_parts(p):
            ids = [corner_id(xy) for xy in list(part.exterior.coords)[:-1]]
            rings.append(ids)
            for a, b in zip(ids, ids[1:] + ids[:1]):
                if a != b:
                    edge_owners.setdefault(tuple(sorted((a, b))), []).append(rid)
        regions.append(
            dict(
                id=rid,
                islandId=island_ids[rid],
                site=site.tolist(),
                geometry=mapping(p),
                cornerRings=rings,
                areaMetres2=float(p.area),
            )
        )
    xy = np.array(corners)
    on_coast = sh.distance(sh.points(xy), land.boundary) < 1e-4
    adjacent = [[] for _ in corners]
    edges = []
    for (a, b), owners in sorted(edge_owners.items()):
        midpoint = (xy[a] + xy[b]) / 2
        length = float(np.linalg.norm(xy[a] - xy[b]))
        cost = length * (
            1
            + 0.35
            * float(gradient_noise(midpoint[0:1], midpoint[1:2], 6000, seed + 100)[0])
        )
        adjacent[a].append((b, cost))
        adjacent[b].append((a, cost))
        edges.append(
            dict(corners=[a, b], regions=sorted(set(owners)), lengthMetres=length)
        )
    distance = np.full(len(corners), np.inf)
    previous = np.full(len(corners), -1, int)
    queue = []
    for i in np.flatnonzero(on_coast):
        distance[i] = 0
        heapq.heappush(queue, (0, int(i)))
    while queue:
        value, i = heapq.heappop(queue)
        if value != distance[i]:
            continue
        for j, cost in adjacent[i]:
            if value + cost < distance[j]:
                distance[j] = value + cost
                previous[j] = i
                heapq.heappush(queue, (distance[j], j))
    if not np.isfinite(distance).all():
        raise ValueError("disconnected polygon corner graph")
    # An explicit editable quantile-to-height curve controls landform coverage.
    # Interpolation is strictly increasing: every inland corner keeps a lower
    # neighbour after reassignment, without solving water or flooding basins.
    inland = np.flatnonzero(~on_coast)
    ordered = inland[np.argsort(distance[inland], kind="stable")]
    quantiles = np.array([0, 0.55, 0.85, 0.97, 1])
    levels = np.array([0, 0.09, 0.30, 0.65, 1]) * peak
    elevation = np.zeros(len(corners))
    u = (np.arange(len(ordered)) + 1) / len(ordered)
    elevation[ordered] = np.interp(u, quantiles, levels)
    features = []
    authored = elevation.copy()
    if composition:
        from terrain_elevation_composition import design_elevation, preserve_downhill

        authored, features = design_elevation(
            xy, land, seed, extent, rolling_relief=rolling_relief
        )
        authored *= peak / 1600
        elevation, previous = preserve_downhill(authored, on_coast, adjacent)
    for r in regions:
        ids = sorted(set(sum(r["cornerRings"], [])))
        value = float(elevation[ids].mean())
        r.update(
            elevationMetres=value,
            terrainClass=(
                "lowland"
                if value < 0.09 * peak
                else "hill" if value < 0.3 * peak else "mountain"
            ),
        )
    product = dict(
        schema=1,
        stage="composed-elevation-v20" if composition else "polygon-elevation-v19",
        seed=seed,
        extent=extent,
        land=mapping(land),
        coastline=coastline,
        method=(
            "composed ranges, hills, plains and plateau caps on a shared polygon graph"
            if composition
            else "relaxed Voronoi corners, weighted coast distance, designed elevation redistribution"
        ),
        corners=[
            dict(
                id=i,
                point=point,
                elevationMetres=float(elevation[i]),
                coast=bool(on_coast[i]),
                lowerNeighbour=int(previous[i]),
                coastDistanceCost=float(distance[i]),
            )
            for i, point in enumerate(corners)
        ],
        edges=edges,
        regions=regions,
        elevationCurve=dict(
            quantiles=quantiles.tolist(),
            heightMetres=levels.tolist(),
            editable=True,
            used=not composition,
        ),
        landforms=features,
        composition=composition,
        rollingRelief=bool(composition and rolling_relief),
        conditioning=dict(
            maximumRaiseMetres=float(np.max(elevation - authored)),
            modifiedCorners=int(np.count_nonzero(elevation - authored > 0.1)),
            purpose="preserve a lower adjacent corner without placing rivers",
        ),
        rivers=[],
        lakes=[],
        cliffs=[],
        cliffsEnabled=False,
        pipeline=dict(
            elevationReady=True,
            rivers="deferred",
            moisture="after river layout",
            biomes="after moisture",
            erosion="optional later detail",
            meshing=False,
        ),
        metrics=dict(
            corners=len(corners),
            regions=len(regions),
            edges=len(edges),
            landAreaMetres2=float(land.area),
            peakMetres=float(elevation.max()),
            terrainAreaMetres2={
                name: sum(
                    r["areaMetres2"] for r in regions if r["terrainClass"] == name
                )
                for name in ["lowland", "hill", "mountain"]
            },
        ),
    )
    return product


def render(product, root, size=1500):
    extent = product["extent"]
    land = sh.geometry.shape(product["land"])
    xy = np.array([c["point"] for c in product["corners"]])
    h = np.array([c["elevationMetres"] for c in product["corners"]])
    axis = np.linspace(0, extent, 1025)
    x, z = np.meshgrid(axis, axis)
    # Internal scalar interpolation only; no render mesh or CDT product.
    field = LinearNDInterpolator(xy, h, fill_value=0)(x, z)
    mask = sh.contains_xy(land, x, z)
    field = np.where(mask, np.maximum(field, 0), 0)
    np.save(root / "elevation-metres.npy", field)
    product["preview"] = dict(
        cellMetres=extent / 1024,
        side=1025,
        rowOrder="south-to-north",
        interpolation="piecewise-linear scalar preview; polygon graph remains authoritative",
    )
    stops = [0, 144, 480, 1040, 1600]
    colors = np.array(
        [
            [144, 178, 116],
            [163, 184, 119],
            [178, 155, 107],
            [144, 132, 116],
            [235, 229, 211],
        ]
    )

    def tint(v):
        return tuple(int(np.interp(v, stops, colors[:, k])) for k in range(3))

    im = Image.new("RGB", (size, size + 145), "#203b3b")
    d = ImageDraw.Draw(im)
    d.rectangle((0, 75, size, size + 75), fill="#295e73")

    def point(p):
        return (p[0] / extent * (size - 1), 75 + (1 - p[1] / extent) * (size - 1))

    for r in product["regions"]:
        for p in sh.get_parts(sh.geometry.shape(r["geometry"])):
            d.polygon(
                [point(v) for v in p.exterior.coords], fill=tint(r["elevationMetres"])
            )
    from terrain_junction_detail import paint_boundaries

    if product.get("noisyEdges", {}).get("junctionMaskRadiusMetres"):
        im = paint_boundaries(
            im, product, point, (size - 1) / extent, colour="#82906f", width=1
        )
        d = ImageDraw.Draw(im)
    d.text(
        (22, 15),
        "ELEVATION PLAN / ranges, plains, hills and plateaus",
        font=font(29),
        fill="white",
    )
    d.text(
        (22, 49),
        f"{extent / 1000:g} km | {len(sh.get_parts(land))} island(s) | rivers and lakes deferred",
        font=font(19),
        fill="white",
    )
    end = 30 + 10000 / extent * (size - 1)
    d.rectangle((20, size + 12, end + 20, size + 67), fill="#203b3b")
    d.line((30, size + 48, end, size + 48), fill="white", width=4)
    d.text((30, size + 18), "10 km", font=font(19), fill="white")
    d.text(
        (22, size + 99),
        "Colour shows chosen elevation, not biomes. Polygon cells guide generation; final borders can be softened.",
        font=font(21),
        fill="white",
    )
    im.save(root / "elevation-regions.png")
    # A smooth elevation/contour view, without simulating eroded rock or water.
    from terrain_landforms import sample

    xx, zz = np.meshgrid(np.linspace(0, extent, size), np.linspace(extent, 0, size))
    value = sample(
        field, np.column_stack([xx.ravel(), zz.ravel()]), extent / 1024
    ).reshape(xx.shape)
    rgb = np.stack([np.interp(value, stops, colors[:, k]) for k in range(3)], axis=-1)
    inside = sh.contains_xy(land, xx, zz)
    rgb[~inside] = [41, 94, 115]
    # Discrete elevation contours make relief readable without fake shadows.
    contour_levels = (
        np.r_[np.arange(25, 301, 25), np.arange(400, 2201, 100)]
        if product.get("rollingRelief")
        else np.arange(100, 2201, 100)
    )
    bands = np.searchsorted(contour_levels, value, side="right")
    edge = np.zeros_like(inside)
    edge[:-1] |= bands[:-1] != bands[1:]
    edge[:, :-1] |= bands[:, :-1] != bands[:, 1:]
    rgb[edge & inside] *= 0.76
    smooth = Image.new("RGB", (size, size + 145), "#203b3b")
    sd = ImageDraw.Draw(smooth)
    smooth.paste(Image.fromarray(np.uint8(rgb)), (0, 75))
    sd.text(
        (22, 15),
        (
            "ELEVATION / rolling lowlands and blended mountain foothills"
            if product.get("rollingRelief")
            else "ELEVATION / composed landforms and 100 m contours"
        ),
        font=font(29),
        fill="white",
    )
    sd.text(
        (22, 49),
        (
            "Contours: 25 m below 300 m, then 100 m. Graph elevation; water and biomes follow later."
            if product.get("rollingRelief")
            else "Graph elevation interpolated for inspection. Rivers, lakes and detail come afterward."
        ),
        font=font(19),
        fill="white",
    )
    sd.line((30, size + 48, end, size + 48), fill="white", width=4)
    sd.text((30, size + 18), "10 km", font=font(19), fill="white")
    sd.text(
        (22, size + 100),
        "Colour: chosen elevation. Sharp ranges, gentle plains and raised plateau caps share one height field.",
        font=font(22),
        fill="white",
    )
    smooth.save(root / "elevation-contours.png")


def generate(output, seed=27, extent=72000.0, count=1080):
    start = time.perf_counter()
    root = Path(output)
    root.mkdir(parents=True, exist_ok=True)
    p = build(seed, extent=extent, count=count, composition=True, land_style="natural")
    from terrain_noisy_region_edges import apply_noisy_edges

    apply_noisy_edges(p)
    p["stage"] = "rolling-elevation-v24"
    render(p, root)
    p["metrics"]["seconds"] = time.perf_counter() - start
    (root / "elevation-map.json").write_text(json.dumps(p, indent=2) + "\n")
    print("PASS polygon elevation: " + json.dumps(p["metrics"]))
    return p


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("output")
    p.add_argument("--seed", type=int, default=27)
    p.add_argument("--extent", type=float, default=72000)
    p.add_argument("--regions", type=int, default=1080)
    a = p.parse_args()
    generate(a.output, a.seed, a.extent, a.regions)
