"""Explicit varying river sections and classified lake ports for downstream maps."""

import numpy as np
import shapely as sh
from shapely.geometry import LineString, Point, mapping
from river_hydraulics import normal_depth


def realize_profiles(water, land):
    for r in water["rivers"]:
        raw = np.array(r["routingPoints"])
        curve = np.array(r["points"])
        oldarc = np.r_[0, np.cumsum(np.linalg.norm(np.diff(raw, axis=0), axis=1))]
        arc = np.r_[0, np.cumsum(np.linalg.norm(np.diff(curve, axis=0), axis=1))]
        coordinate = arc / max(arc[-1], 1) * oldarc[-1]
        q = np.interp(coordinate, oldarc, r["dischargeProfileCumecs"])
        width = 1.5 + 6 * q**0.45
        depth = np.array(
            [normal_depth(float(flow), float(w), 0.002)[0] for flow, w in zip(q, width)]
        )
        r["surfaceProfileMetres"] = np.interp(
            coordinate, oldarc, r["waterProfileMetres"]
        ).tolist()
        r["curveDischargeCumecs"] = q.tolist()
        r["widthProfileMetres"] = width.tolist()
        r["depthProfileMetres"] = depth.tolist()
        r["bedProfileMetres"] = (np.array(r["surfaceProfileMetres"]) - depth).tolist()
        r["width"] = float(width[-1])
        r["depth"] = float(depth[-1])
        r["computedDischarge"] = normal_depth(float(q[-1]), float(width[-1]), 0.002)[1]
        r["hydraulicModel"] = (
            "rectangular Manning sizing at reference slope 0.002; actual terrain water profile exported separately; backwater not solved"
        )
        r["referenceSizingSlope"] = 0.002
        # Variable-width geometry follows each profile segment; all vertices stay
        # in world metres, independent of the minimum cartographic line width.
        pieces = []
        for a, b, wa, wb in zip(curve[:-1], curve[1:], width[:-1], width[1:]):
            tangent = b - a
            length = np.linalg.norm(tangent)
            if length < 1e-8:
                continue
            normal = np.array([-tangent[1], tangent[0]]) / length
            from shapely.geometry import Polygon

            pieces.append(
                Polygon(
                    [
                        a + normal * wa / 2,
                        b + normal * wb / 2,
                        b - normal * wb / 2,
                        a - normal * wa / 2,
                    ]
                )
            )
        pieces.extend(
            Point(p).buffer(float(w) / 2, quad_segs=4) for p, w in zip(curve, width)
        )
        r["waterGeometry"] = mapping(sh.union_all(pieces).intersection(land))
    for lake in water["lakes"]:
        geom = sh.geometry.shape(lake["geometry"])
        for port in lake["ports"]:
            river = next(r for r in water["rivers"] if r["id"] == port["river"])
            line = LineString(river["points"])
            d = port["distanceAlongRiverMetres"]
            before = geom.covers(line.interpolate(max(0, d - 0.05)))
            after = geom.covers(line.interpolate(min(line.length, d + 0.05)))
            port["kind"] = (
                "inlet"
                if after and not before
                else "outlet" if before and not after else "touch"
            )
        lake["inletPorts"] = [p for p in lake["ports"] if p["kind"] == "inlet"]
        lake["outletPorts"] = [p for p in lake["ports"] if p["kind"] == "outlet"]
