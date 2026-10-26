"""Solve lake elevation contracts on an unstructured scalar field, without meshes.

The output is map data: a shoreline level, a constructive bed-depth rule, river
profile knots and conservative bank/cut/storage bounds. No surface is generated.
"""

from copy import deepcopy
from dataclasses import dataclass
import math
import numpy as np
import shapely as sh
from shapely.geometry import Polygon, Point, MultiPoint
from shapely.ops import unary_union
from scipy.spatial import Delaunay


def polygon_parts(g):
    if g.is_empty:
        return []
    if g.geom_type == "Polygon":
        return [g]
    return [p for child in getattr(g, "geoms", ()) for p in polygon_parts(child)]


@dataclass(frozen=True)
class BasinSettings:
    bank_width: float = 12.0
    freeboard: float = 0.35
    maximum_cut: float = 24.0
    minimum_depth: float = 1.5
    maximum_depth: float = 5.0
    volume_layers: int = 64

    def validate(self):
        values = (
            self.bank_width,
            self.freeboard,
            self.maximum_cut,
            self.minimum_depth,
            self.maximum_depth,
        )
        if not all(math.isfinite(v) and v > 0 for v in values):
            raise ValueError("basin settings must be positive finite values")
        if (
            self.maximum_depth < self.minimum_depth
            or not 8 <= self.volume_layers <= 1024
        ):
            raise ValueError("invalid basin depth/layer budget")


class ScalarEnvelope:
    """Exact min/max of the specified piecewise-linear analysis field on polygons.

    The Delaunay adjacency belongs to the scalar interpolator, not a terrain mesh
    product. It is never exported or displayed. Reject missing interpolation support.
    """

    def __init__(self, sites):
        sites = np.asarray(sites, dtype=float)
        if (
            sites.ndim != 2
            or sites.shape[1] != 3
            or len(sites) < 3
            or len(sites) > 10000
            or not np.isfinite(sites).all()
        ):
            raise ValueError("invalid unstructured scalar probes")
        # Conflicting duplicate probes have no single-valued scalar interpretation.
        unique = {}
        for x, z, h in sites:
            key = (float(x), float(z))
            if key in unique and abs(unique[key] - h) > 1e-7:
                raise ValueError("conflicting scalar probes")
            unique[key] = float(h)
        ordered = sorted(unique)
        self.points = np.array(ordered)
        values = np.array([unique[k] for k in ordered])
        triangulation = Delaunay(self.points)
        self.polygons = []
        self.planes = []
        for indices in triangulation.simplices:
            xy = self.points[indices]
            self.polygons.append(Polygon(xy))
            self.planes.append(
                np.linalg.solve(np.column_stack([xy, np.ones(3)]), values[indices])
            )
        self.tree = sh.STRtree(self.polygons)
        self.hull = MultiPoint(self.points).convex_hull

    def bounds(self, region):
        if region.is_empty or not region.is_valid or region.area <= 0:
            raise ValueError("empty or invalid scalar query domain")
        if region.difference(self.hull).area > 1e-6:
            raise ValueError("missing scalar support for basin domain")
        lo, hi = float("inf"), -float("inf")
        for index in self.tree.query(region, predicate="intersects"):
            clipped = region.intersection(self.polygons[int(index)])
            plane = self.planes[int(index)]
            for polygon in polygon_parts(clipped):
                for ring in [polygon.exterior, *polygon.interiors]:
                    xy = np.asarray(ring.coords)
                    h = xy @ plane[:2] + plane[2]
                    lo = min(lo, float(h.min()))
                    hi = max(hi, float(h.max()))
        if not math.isfinite(lo) or not math.isfinite(hi):
            raise ValueError("missing scalar support")
        return lo, hi


def storage_bounds(shore, depth, run, layers):
    """Layer-cake volume bounds for depth=D*min(distance(shore)/run,1)."""
    area = [shore.buffer(-run * i / layers).area for i in range(layers + 1)]
    lower = depth / layers * sum(area[1:])
    upper = depth / layers * sum(area[:-1])
    return {
        "lowerCubicMetres": lower,
        "upperCubicMetres": upper,
        "estimateCubicMetres": 0.5 * (lower + upper),
        "layers": layers,
    }


def solve_lake_basins(hydrology, sites, land, settings=BasinSettings()):
    settings.validate()
    candidate = deepcopy(hydrology)
    field = ScalarEnvelope(sites)
    reaches = {r["id"]: r for r in candidate["reaches"]}
    lakes = candidate["lakes"]
    if len(reaches) != len(candidate["reaches"]) or len(
        {l["id"] for l in lakes}
    ) != len(lakes):
        raise ValueError("duplicate basin or reach identity")
    for reach in reaches.values():
        if (
            reach["geometry"].is_empty
            or reach["geometry"].length <= 1e-7
            or not reach["geometry"].is_simple
        ):
            raise ValueError("invalid river curve")
        if (
            not all(
                math.isfinite(reach[k]) for k in ("waterFrom", "waterTo", "discharge")
            )
            or reach["discharge"] < 0
        ):
            raise ValueError("invalid river scalar")
    for lake in lakes:
        if not all(
            math.isfinite(lake[k])
            for k in ("inflow", "localRunoff", "outflow", "storageChangeCumecs")
        ):
            raise ValueError("nonfinite lake flow")
    solved = []
    for lake in lakes:
        shore = lake["geometry"]
        outlet = lake["outlet"]
        if (
            shore.geom_type != "Polygon"
            or not shore.is_valid
            or shore.area <= 0
            or not land.covers(shore)
        ):
            raise ValueError("invalid lake shoreline")
        if any(shore.intersects(p["geometry"]) for p in solved):
            raise ValueError("overlapping lake domains")
        if lake["receiverReach"] not in reaches or not lake["inlets"]:
            raise ValueError("missing lake ports")
        receiver = reaches[lake["receiverReach"]]
        curve = receiver["geometry"]
        if outlet.distance(shore.boundary) > 1e-6 or outlet.distance(curve) > 1e-6:
            raise ValueError("outlet is not on both shore and receiver")
        fraction = curve.project(outlet) / curve.length
        level = receiver["waterFrom"] * (1 - fraction) + receiver["waterTo"] * fraction
        if not math.isfinite(level):
            raise ValueError("nonfinite lake level")
        for port in lake["inlets"]:
            if (
                port["reach"] not in reaches
                or port["point"].distance(shore.boundary) > 1e-6
                or port["point"].distance(reaches[port["reach"]]["geometry"]) > 1e-6
            ):
                raise ValueError("invalid lake inlet")
            port["waterLevelMetres"] = level
        if (
            abs(
                lake["inflow"]
                + lake["localRunoff"]
                - lake["outflow"]
                - lake["storageChangeCumecs"]
            )
            > 1e-7
        ):
            raise ValueError("unbalanced lake flow ledger")
        collar = shore.buffer(settings.bank_width).difference(shore)
        if not land.covers(shore.buffer(settings.bank_width)):
            raise ValueError("lake bank reservation reaches ocean")
        minimum_bank, _ = field.bounds(collar)
        if minimum_bank < level + settings.freeboard:
            raise ValueError("uncontrolled lake flooding beyond bank collar")
        depth = min(
            settings.maximum_depth,
            settings.minimum_depth + 0.55 * max(0, lake["outflow"]) ** 0.25,
        )
        low, high = field.bounds(shore)
        maximum_cut = max(0.0, high - level + depth)
        if maximum_cut > settings.maximum_cut:
            raise ValueError("lake basin cut exceeds declared map budget")
        run = max(1.0, math.sqrt(shore.area / math.pi) * 0.65)
        lake.update(
            {
                "waterLevelMetres": level,
                "outletWaterLevelMetres": level,
                "shorelineLevels": [
                    [float(x), float(z), level] for x, z in shore.exterior.coords
                ],
                "bedRule": {
                    "type": "shore-distance",
                    "maximumDepthMetres": depth,
                    "rampDistanceMetres": run,
                },
                "storage": storage_bounds(shore, depth, run, settings.volume_layers),
                "bankReservation": shore.buffer(settings.bank_width),
                "bankMinimumNaturalHeightMetres": minimum_bank,
                "bankFreeboardMetres": minimum_bank - level,
                "maximumCutBoundMetres": maximum_cut,
                "maximumFillBoundMetres": max(0.0, level - low),
                "naturalHeightBoundsMetres": [low, high],
                "status": "map-basin-solved",
                "requiresElevationSolve": False,
                "requiresPhysicalRealization": True,
            }
        )
        solved.append(lake)
    # Solve shared node levels once, then split every affected reach at all ports.
    node_levels = {}
    for r in candidate["reaches"]:
        for node, p, h in [
            (r["from"], r["geometry"].coords[0], r["waterFrom"]),
            (r["to"], r["geometry"].coords[-1], r["waterTo"]),
        ]:
            owners = [l for l in solved if l["geometry"].covers(Point(p))]
            value = owners[0]["waterLevelMetres"] if owners else h
            if node in node_levels and abs(node_levels[node] - value) > 1e-7:
                raise ValueError("incompatible water level at shared river node")
            node_levels[node] = value
    for r in candidate["reaches"]:
        curve = r["geometry"]
        length = curve.length
        knots = {0.0: node_levels[r["from"]], length: node_levels[r["to"]]}
        for lake in solved:
            contacts = curve.intersection(lake["geometry"].boundary)
            for contact in getattr(contacts, "geoms", [contacts]):
                if contact.geom_type == "Point":
                    knots[curve.project(contact)] = lake["waterLevelMetres"]
                elif not contact.is_empty:
                    raise ValueError("river overlaps lake shoreline")
        profile = sorted(knots.items())
        if any(b[1] > a[1] + 1e-7 for a, b in zip(profile, profile[1:])):
            raise ValueError("basin produces uphill connecting river profile")
        r["waterFrom"] = profile[0][1]
        r["waterTo"] = profile[-1][1]
        r["waterProfile"] = [
            {"distanceMetres": d, "waterLevelMetres": h} for d, h in profile
        ]
    candidate["basinSolver"] = {
        "version": 1,
        "field": "piecewise-linear unstructured scalar probes",
        "settings": settings.__dict__,
        "geometryGenerated": False,
        "hydraulicSectionsRequireProfileRefresh": True,
    }
    return candidate
