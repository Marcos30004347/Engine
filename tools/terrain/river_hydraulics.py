"""Map-stage rectangular normal-flow sections; explicit still-water exceptions."""

from copy import deepcopy
import math


def normal_depth(discharge, width, slope, roughness=0.035):
    if (
        not all(math.isfinite(v) for v in (discharge, width, slope, roughness))
        or discharge < 0
        or min(width, slope, roughness) <= 0
    ):
        raise ValueError("invalid normal-flow inputs")

    if discharge == 0:
        return 0.0, 0.0

    def flow(depth):
        area = width * depth
        radius = area / (width + 2 * depth)
        return area * radius ** (2 / 3) * math.sqrt(slope) / roughness

    lo, hi = 0.0, 64.0
    if flow(hi) < discharge:
        raise ValueError("normal-depth budget exceeded")
    for _ in range(64):
        mid = (lo + hi) / 2
        if flow(mid) < discharge:
            lo = mid
        else:
            hi = mid
    depth = (lo + hi) / 2
    return depth, flow(depth)


def refresh_reach(r):
    q = r["discharge"]
    if not math.isfinite(q) or q < 0:
        raise ValueError("invalid discharge")
    width = max(1.5, 6 * q**0.45)
    profile = r.get(
        "waterProfile",
        [
            {"distanceMetres": 0, "waterLevelMetres": r["waterFrom"]},
            {"distanceMetres": r["geometry"].length, "waterLevelMetres": r["waterTo"]},
        ],
    )
    sections = []
    for a, b in zip(profile, profile[1:]):
        length = b["distanceMetres"] - a["distanceMetres"]
        drop = a["waterLevelMetres"] - b["waterLevelMetres"]
        if length <= 0 or drop < -1e-7:
            raise ValueError("invalid section profile")
        section = {
            "fromDistanceMetres": a["distanceMetres"],
            "toDistanceMetres": b["distanceMetres"],
            "discharge": q,
            "widthMetres": width,
        }
        if drop <= 1e-7:
            section.update(kind="lake-backwater", requiresBackwaterSolve=True)
        else:
            slope = drop / length
            depth, computed = normal_depth(q, width, slope)
            section.update(
                kind="normal-flow",
                slope=slope,
                depthMetres=depth,
                areaMetres2=width * depth,
                velocityMetresPerSecond=q / (width * depth) if q else 0.0,
                manningRoughness=0.035,
                computedDischarge=computed,
                requiresBackwaterSolve=False,
            )
        sections.append(section)
    flowing = [s for s in sections if s["kind"] == "normal-flow"]
    r["hydraulicSections"] = sections
    r["width"] = width
    if flowing:
        representative = max(
            flowing, key=lambda s: s["toDistanceMetres"] - s["fromDistanceMetres"]
        )
        r["depth"] = representative["depthMetres"]
        r["velocity"] = representative["velocityMetresPerSecond"]
    r["hydraulicSectionNeedsRefresh"] = False
    r["requiresBackwaterSolve"] = any(s["requiresBackwaterSolve"] for s in sections)
    return r


def refresh_hydrology(hydrology):
    result = deepcopy(hydrology)
    for r in result["reaches"]:
        refresh_reach(r)
    result["basinSolver"]["hydraulicSectionsRequireProfileRefresh"] = False
    result["hydraulicModel"] = {
        "version": 1,
        "type": "rectangular normal-flow segments",
        "widthRule": "max(1.5, 6 * discharge^0.45) metres",
        "roughness": 0.035,
        "backwaterSolved": False,
        "geometryGenerated": False,
    }
    return result
