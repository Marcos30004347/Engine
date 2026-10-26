"""Terrain-derived drainage, conserved runoff, retained lakes and ocean outlets."""

import heapq
import numpy as np
from scipy.ndimage import binary_erosion, label, find_objects
from shapely.geometry import LineString, Point, mapping
from shapely.ops import nearest_points
import shapely as sh
from terrain_landforms import cover_polygon
from river_hydraulics import normal_depth

NEIGHBOURS = [(-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1)]
YEAR_SECONDS = 365.25 * 86400


def route(height, mask, style="steepest"):
    if style not in ("steepest", "distributed"):
        raise ValueError("unknown routing style")
    """Priority-Flood spill levels plus steepest descent; rank resolves flats."""
    n, m = height.shape
    filled = height.copy()
    parent = np.full(height.shape, -1, np.int32)
    rank = np.full(height.shape, -1, np.int32)
    seen = ~mask.copy()
    boundary = (~mask) & (
        ~binary_erosion(~mask, structure=np.ones((3, 3)), border_value=1)
    )
    # A spatially varying travel cost resolves equal spill levels. Sorting flats
    # only by raster index produces axial/diagonal outlet cuts kilometres long.
    zz_grid, xx_grid = np.mgrid[:n, :m]
    resistance = np.exp(
        0.65 * np.sin(xx_grid * 0.083 + zz_grid * 0.037)
        + 0.45 * np.sin(xx_grid * 0.031 - zz_grid * 0.091 + 1.7)
    )
    heap = [
        (float(height[z, x]), 0.0, int(z * m + x)) for z, x in np.argwhere(boundary)
    ]
    heapq.heapify(heap)
    order = []
    while heap:
        level, path_cost, i = heapq.heappop(heap)
        z, x = divmod(i, m)
        rank[z, x] = len(order)
        order.append(i)
        for dz, dx in NEIGHBOURS:
            zz, xx = z + dz, x + dx
            if 0 <= zz < n and 0 <= xx < m and not seen[zz, xx]:
                seen[zz, xx] = True
                parent[zz, xx] = i
                filled[zz, xx] = max(level, height[zz, xx])
                step_cost = (
                    (path_cost + np.hypot(dx, dz) * resistance[zz, xx])
                    if style == "distributed"
                    else 0.0
                )
                heapq.heappush(heap, (filled[zz, xx], step_cost, zz * m + xx))
    if np.any(rank[mask] < 0):
        raise ValueError("land has no ocean-connected flood boundary")
    # Priority queue discovers a safe path. Prefer steepest physical descent on
    # nonflat ground to avoid routing according to arbitrary queue tie order.
    best = np.zeros_like(height)
    ids = np.arange(n * m).reshape(n, m)
    for dz, dx in NEIGHBOURS:
        hh = np.roll(filled, (-dz, -dx), (0, 1))
        rr = np.roll(rank, (-dz, -dx), (0, 1))
        ii = np.roll(ids, (-dz, -dx), (0, 1))
        slope = (filled - hh) / np.hypot(dx, dz)
        score = slope
        if style == "distributed":
            # Deterministic weighted selection amongst descending neighbours.
            # Break long D8 quantization runs while preserving rank and downhill flow.
            hashed = np.sin(ids * 12.9898 + (dz * 3 + dx + 5) * 78.233) * 43758.5453
            uniform = np.clip(hashed - np.floor(hashed), 1e-6, 1 - 1e-6)
            score = np.maximum(slope, 1e-8) / (-np.log(uniform)) ** 0.5
        ok = mask & (rr >= 0) & (rr < rank) & (score > best + 1e-12)
        if dz < 0:
            ok[0] = False
        if dz > 0:
            ok[-1] = False
        if dx < 0:
            ok[:, 0] = False
        if dx > 0:
            ok[:, -1] = False
        parent[ok] = ii[ok]
        best[ok] = score[ok]
    # Two diagonal receivers in one raster square can cross without sharing a
    # node. Redirect one source through a lower-rank cardinal corner, making
    # the join explicit before runoff accumulation and visible graph extraction.
    pf, rf, ff = parent.ravel(), rank.ravel(), filled.ravel()
    a = ids[:-1, :-1].ravel()
    b = a + 1
    c = a + m
    d = c + 1
    conflict = ((pf[a] == d) | (pf[d] == a)) & ((pf[b] == c) | (pf[c] == b))
    for aa, bb, cc, dd in zip(a[conflict], b[conflict], c[conflict], d[conflict]):
        s1 = aa if pf[aa] == dd else dd
        s2 = bb if pf[bb] == cc else cc
        options = []
        for source, candidates in [(s1, (bb, cc)), (s2, (aa, dd))]:
            for target in candidates:
                if (
                    rf[target] >= 0
                    and rf[target] < rf[source]
                    and ff[target] <= ff[source]
                ):
                    options.append(
                        (
                            ff[source] - ff[target],
                            -int(source),
                            int(source),
                            int(target),
                        )
                    )
        if not options:
            raise ValueError("cannot resolve diagonal drainage crossing")
        _, _, source, target = max(options)
        pf[source] = target
    return filled, parent, rank, np.array(order, dtype=np.int32)


def accumulate(local, parent, order):
    flow = local.ravel().copy()
    p = parent.ravel()
    for i in order[::-1]:
        if p[i] >= 0:
            flow[p[i]] += flow[i]
    return flow.reshape(local.shape)


def rounded(points, passes=2):
    p = np.array(points, float)
    for _ in range(passes):
        a = 0.75 * p[:-1] + 0.25 * p[1:]
        b = 0.25 * p[:-1] + 0.75 * p[1:]
        middle = np.stack([a, b], axis=1).reshape(-1, 2)
        p = np.vstack([p[0], middle, p[-1]])
    return p


def build_drainage(
    height,
    mask,
    cell,
    land,
    runoff_mm,
    route_style="steepest",
    source_height=350,
    max_sources=24,
    max_lakes=6,
    source_spacing=2500,
    minimum_source_area=0.2e6,
    minimum_lake_area=80000,
    source_selection="threshold-heads",
):
    filled, parent, rank, order = route(height, mask, route_style)
    local = runoff_mm * mask * cell**2 / 1000 / YEAR_SECONDS
    q = accumulate(local, parent, order)
    area = accumulate(mask * cell**2, parent, order)
    n = len(height)
    p = parent.ravel()
    hf = height.ravel()
    mf = mask.ravel()
    qf = q.ravel()
    # Retain substantive physical depressions. Small pits are explicitly filled
    # in the conditioning field, not silently treated as dry upstream channels.
    depth = np.where(mask, filled - height, 0)
    labels, count = label(depth > 0.5, np.ones((3, 3)))
    sizes = np.bincount(labels.ravel())
    maxdepth = np.zeros(count + 1)
    np.maximum.at(maxdepth, labels.ravel(), depth.ravel())
    peak_flow = np.zeros(count + 1)
    np.maximum.at(peak_flow, labels.ravel(), q.ravel())
    valid = np.flatnonzero(
        (sizes * cell**2 >= minimum_lake_area)
        & (sizes * cell**2 <= 1.2e6)
        & (maxdepth >= 2)
        & (maxdepth <= 35)
        & (peak_flow > 0.01)
    )
    valid = valid[valid != 0] if max_lakes > 0 else []
    lake_ids = np.zeros_like(parent)
    lakes = []
    centres = []
    # A declared retention recipe, rather than classifying every noise pit as
    # a permanent lake. Other basins are filled in the conditioning surface.
    for lid in sorted(valid, key=lambda i: -peak_flow[i]):
        at = labels == lid
        if np.ptp(filled[at]) > 1e-6:
            continue
        zz, xx = np.nonzero(at)
        centre = np.array([xx.mean(), zz.mean()]) * cell
        if any(np.linalg.norm(centre - c) < 2500 for c in centres):
            continue
        centres.append(centre)
        lake_id = len(lakes) + 1
        lake_ids[at] = lake_id
        geometry = cover_polygon(at.astype(float), cell, land, 0.5)
        lakes.append(
            dict(
                id=lake_id,
                geometry=mapping(geometry),
                levelMetres=float(filled[at][0]),
                areaMetres2=float(at.sum() * cell**2),
                maximumDepthMetres=float(depth[at].max()),
                storageCubicMetres=float(depth[at].sum() * cell**2),
            )
        )
        if len(lakes) == max_lakes:
            break
    conditioned = np.where((lake_ids == 0) & mask, filled, height)
    # Every visible source is a drainage head on high ground, with a meaningful
    # contributing area and sustained positive annual water yield.
    eligible = (area >= minimum_source_area) & mask & (q > 0.002)
    upstream = np.zeros(n * n, np.int32)
    ee = np.flatnonzero(eligible.ravel())
    np.add.at(upstream, p[ee], 1)
    head_filter = (
        upstream == 0 if source_selection == "threshold-heads" else np.ones(n * n, bool)
    )
    candidates = np.flatnonzero(
        eligible.ravel() & head_filter & (hf >= source_height) & (lake_ids.ravel() == 0)
    )
    candidates = sorted(
        candidates,
        key=(
            (lambda i: (-qf[i], int(i)))
            if source_selection == "threshold-heads"
            else (lambda i: (-hf[i], -qf[i], int(i)))
        ),
    )
    sources = []
    network = np.zeros(n * n, bool)
    for i in candidates:
        if network[i]:
            continue
        pos = np.array([i % n, i // n]) * cell
        if any(
            np.linalg.norm(pos - np.array(s["point"])) < source_spacing for s in sources
        ):
            continue
        sources.append(
            dict(
                id=len(sources) + 1,
                cell=int(i),
                point=pos.tolist(),
                elevationMetres=float(hf[i]),
                catchmentAreaMetres2=float(area.ravel()[i]),
            )
        )
        j = i
        while mf[j]:
            network[j] = True
            j = int(p[j])
        network[j] = True
        if len(sources) >= max_sources:
            break
    if not sources:
        raise ValueError("no high-ground river sources for this climate")
    degree = np.zeros(n * n, np.int32)
    edges = np.flatnonzero(network & mf)
    np.add.at(degree, p[edges], 1)
    nodes = network & ((degree != 1) | (~mf))
    nodes[[s["cell"] for s in sources]] = True
    rivers = []
    graph_nodes = []
    for start in np.flatnonzero(nodes & mf):
        ids = [int(start)]
        end = int(p[start])
        ids.append(end)
        while not nodes[end]:
            end = int(p[end])
            ids.append(end)
        points = np.column_stack([np.array(ids) % n, np.array(ids) // n]) * cell
        if not mf[end]:
            last = LineString(points[-2:]).intersection(land.boundary)
            if last.geom_type == "Point":
                points[-1] = [last.x, last.y]
            else:
                pp = nearest_points(Point(points[-2]), land.boundary)[1]
                points[-1] = [pp.x, pp.y]
        curved = rounded(points)
        # Remain in the raw drainage corridor and on land. Endpoint clipping is
        # exact; no arbitrary sinusoidal meanders are added across divides.
        raw = LineString(points)
        candidate = LineString(curved)
        if (
            not candidate.is_simple
            or not land.buffer(0.01).covers(candidate)
            or not raw.buffer(cell * 0.6).covers(candidate)
        ):
            curved = points
        discharges = qf[ids].copy()
        # At a confluence the tributary owns its incoming flow; the downstream
        # reach starts with the sum at the common node, avoiding double counts.
        discharges[-1] = qf[ids[-2]]
        water = filled.ravel()[ids].copy()
        water[-1] = water[-1] if mf[end] else 0
        qout = float(discharges[-1])
        width = max(1.5, 5 * qout**0.45)
        gradient = max(1e-4, (water[0] - water[-1]) / max(raw.length, 1))
        depth_m, computed = normal_depth(qout, width, gradient)
        rivers.append(
            dict(
                id=len(rivers) + 1,
                fromNode=int(start),
                toNode=int(end),
                points=curved.tolist(),
                routingCells=ids,
                routingPoints=points.tolist(),
                waterProfileMetres=water.tolist(),
                dischargeProfileCumecs=discharges.tolist(),
                discharge=qout,
                width=width,
                depth=depth_m,
                computedDischarge=computed,
                energySlope=gradient,
                endsAtOcean=bool(not mf[end]),
                hydraulicModel="representative rectangular Manning section; lake backwater deferred",
            )
        )
    # Reject smoothing that introduces new crossings; raw D8 paths are the
    # authoritative topology. Diagonal crossings cannot have strict descent
    # in both directions for a steepest-descent receiver field.
    rawlines = [LineString(r["routingPoints"]) for r in rivers]
    for i, r in enumerate(rivers):
        line = LineString(r["points"])
        for k, other in enumerate(rivers):
            if k == i or {r["fromNode"], r["toNode"]} & {
                other["fromNode"],
                other["toNode"],
            }:
                continue
            if line.crosses(LineString(other["points"])):
                r["points"] = r["routingPoints"]
                other["points"] = other["routingPoints"]
                line = rawlines[i]
    for node in np.flatnonzero(nodes):
        incoming = sum(qf[i] for i in np.flatnonzero((p == node) & network))
        # q includes unrendered drainage. Explicit local/unmapped contribution
        # makes every graph junction balance without throwing this water away.
        outgoing = float(qf[node])
        increment = outgoing - incoming
        graph_nodes.append(
            dict(
                id=int(node),
                incomingCumecs=float(incoming),
                incrementalCumecs=float(increment),
                outgoingCumecs=outgoing,
                ocean=bool(not mf[node]),
            )
        )
    lake_flat = lake_ids.ravel()
    all_land = np.flatnonzero(mf)
    for lake in lakes:
        lid = lake["id"]
        inside = lake_flat == lid
        entering = all_land[(~inside[all_land]) & (lake_flat[p[all_land]] == lid)]
        leaving = all_land[inside[all_land] & (lake_flat[p[all_land]] != lid)]
        inflow = float(qf[entering].sum())
        outflow = float(qf[leaving].sum())
        localflow = float(local.ravel()[inside].sum())
        lake.update(
            inflowCumecs=inflow,
            localRunoffCumecs=localflow,
            outflowCumecs=outflow,
            balanceResidualCumecs=inflow + localflow - outflow,
            connectedRiverIds=[],
        )
        geometry = sh.geometry.shape(lake["geometry"])
        ports = []
        for r in rivers:
            line = LineString(r["points"])
            if line.intersects(geometry):
                lake["connectedRiverIds"].append(r["id"])
                hits = line.intersection(geometry.boundary)
                for pt in sh.get_parts(hits):
                    if pt.geom_type == "Point":
                        ports.append(
                            dict(
                                river=r["id"],
                                point=[pt.x, pt.y],
                                distanceAlongRiverMetres=line.project(pt),
                            )
                        )
        lake["ports"] = ports
    ocean_out = float(q[~mask].sum())
    input_flow = float(local.sum())
    stats = dict(
        sources=len(sources),
        reaches=len(rivers),
        oceanMouths=sum(r["endsAtOcean"] for r in rivers),
        lakes=len(lakes),
        lakesWithVisibleRivers=sum(bool(l["connectedRiverIds"]) for l in lakes),
        totalRunoffCumecs=input_flow,
        oceanDischargeCumecs=ocean_out,
        massResidualCumecs=input_flow - ocean_out,
        maximumLakeDepthMetres=max([l["maximumDepthMetres"] for l in lakes] or [0]),
        conditioningFillCubicMetres=float(
            ((conditioned - height) * mask).sum() * cell**2
        ),
    )
    return dict(
        rivers=rivers, lakes=lakes, sources=sources, nodes=graph_nodes, metrics=stats
    ), dict(
        conditioned_height=conditioned,
        filled_level=filled,
        receiver=parent,
        rank=rank,
        accumulated_discharge=q,
        catchment_area=area,
        lake_id=lake_ids,
        local_runoff_cumecs=local,
    )
