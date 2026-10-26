"""Initial regional director: society seats, contiguous claims and wilderness.

No population/supply feasibility or local settlement generation is claimed here.
"""

import argparse, json, heapq
from pathlib import Path
import numpy as np
from shapely.geometry import shape, mapping
import shapely as sh

PROFILES = [
    ("agrarian", "kingdom", {"grassland": 1.0, "woodland": 0.65, "dry-scrub": 0.4}),
    ("forest", "kingdom", {"woodland": 1.0, "rainforest": 1.0, "grassland": 0.2}),
    ("desert", "city-state", {"desert": 1.0, "dry-scrub": 0.8}),
    ("mountain", "kingdom", {"cold-heath": 0.7, "woodland": 0.2, "dry-scrub": 0.2}),
    ("seafaring", "maritime-state", {}),
]


def generate(product, claimed_fraction=1.0):
    if not 0 < claimed_fraction <= 1:
        raise ValueError("claimed fraction must be greater than zero and at most one")
    regions = product["regions"]
    n = len(regions)
    centres = np.array([r["site"] for r in regions])
    heights = np.array([r["elevationMetres"] for r in regions])
    areas = np.array([r["areaMetres2"] for r in regions])
    coast_length = np.zeros(n)
    for edge in product["edges"]:
        if len(edge["regions"]) == 1:
            coast_length[edge["regions"][0]] += edge["lengthMetres"]
    coastal = coast_length > 0
    land = shape(product["land"])
    coast_distance = sh.distance(sh.points(centres), land.boundary)
    mountain = heights >= 480
    # Preference is separate from capital spacing and physical feasibility.
    suitability = {}
    for name, _, weights in PROFILES:
        score = np.array([weights.get(r["biome"], 0.02) for r in regions])
        if name == "mountain":
            score = 0.08 + 0.92 * np.clip(heights / 1100, 0, 1)
        elif name == "seafaring":
            score = 0.12 + 0.65 * coastal + 0.23 * np.exp(-coast_distance / 2500)
        else:
            score *= np.clip(1 - heights / 1300, 0, 1)
        suitability[name] = score
    adjacency = [[] for _ in regions]
    for edge in product["edges"]:
        if len(edge["regions"]) == 2:
            a, b = edge["regions"]
            distance = float(np.linalg.norm(centres[a] - centres[b]))
            cost = distance / 4000
            adjacency[a].append((b, cost))
            adjacency[b].append((a, cost))
    seats = []
    societies = []
    scores = []
    omitted = []
    # Select specialist opportunities first; generic lowlands cannot crowd them out.
    priority = {"mountain": 0, "seafaring": 1, "desert": 2, "forest": 3, "agrarian": 4}
    for name, form, weights in sorted(
        PROFILES, key=lambda profile: priority[profile[0]]
    ):
        base_score = suitability[name]
        score = base_score.copy()
        if name == "mountain":
            score[~mountain] = -1
        if name == "seafaring":
            score[~coastal] = -1
        if seats:
            distance = np.min(
                np.linalg.norm(centres[:, None] - centres[seats][None], axis=2), axis=1
            )
            score *= np.clip(distance / 15000, 0, 1)
            score[distance < 5000] = -1
        seat = int(np.argmax(score))
        if score[seat] <= 0:
            omitted.append(
                dict(profile=name, reason="no eligible separated seat candidate")
            )
            continue
        seats.append(seat)
        scores.append(base_score)
        societies.append(
            dict(
                id=len(seats) - 1,
                profile=name,
                form=form,
                capitalCandidateRegion=seat,
                point=centres[seat].tolist(),
                status="provisional",
                mineralSurvey=(
                    "not available; mountain profile uses elevation proxy"
                    if name == "mountain"
                    else None
                ),
            )
        )
    owner = np.full(n, -1, int)
    distance = np.full(n, np.inf)
    claimed = np.zeros(len(seats))
    full_coverage = claimed_fraction == 1.0
    budget = float("inf") if full_coverage else areas.sum() * claimed_fraction / max(len(seats), 1)
    attempted = [set() for _ in regions]
    budget_rejected = [set() for _ in regions]
    queue = []
    for polity, seat in enumerate(seats):
        heapq.heappush(queue, (0.0, polity, seat))
    while queue or (full_coverage and np.any(owner < 0)):
        if not queue:
            # A disconnected land component needs a local polity, not an
            # invented sea connection or an unowned hole in the map.
            remaining = np.flatnonzero(owner < 0)
            shore = remaining[coastal[remaining]]
            if len(shore):
                name = "seafaring"
                seat = int(shore[np.argmax(suitability[name][shore])])
                form = "maritime-state"
            else:
                name = max(suitability, key=lambda key: float(suitability[key][remaining].max()))
                seat = int(remaining[np.argmax(suitability[name][remaining])])
                form = next(form for profile, form, _ in PROFILES if profile == name)
            polity = len(societies)
            societies.append(dict(id=polity,profile=name,form=form,
                capitalCandidateRegion=seat,point=centres[seat].tolist(),status="provisional",
                creationReason="independent polity for disconnected unassigned land component"))
            seats.append(seat);scores.append(suitability[name]);claimed=np.append(claimed,0.)
            heapq.heappush(queue,(0.,polity,seat))
        cost, polity, region = heapq.heappop(queue)
        attempted[region].add(polity)
        if owner[region] >= 0:
            continue
        if claimed[polity] + areas[region] > budget:
            budget_rejected[region].add(polity)
            continue
        owner[region] = polity
        distance[region] = cost
        claimed[polity] += areas[region]
        for other, travel in sorted(adjacency[region]):
            if owner[other] < 0:
                heapq.heappush(
                    queue,
                    (
                        cost
                        + travel
                        * (
                            1
                            + abs(heights[region] - heights[other])
                            / (
                                900
                                if societies[polity]["profile"] == "mountain"
                                else 250
                            )
                        )
                        * (2.0 - 1.7 * scores[polity][other]),
                        polity,
                        other,
                    ),
                )
    for i, s in enumerate(societies):
        s["regionIds"] = np.flatnonzero(owner == i).tolist()
        s["claimedAreaMetres2"] = float(claimed[i])
        owned = owner == i
        s["mountainAreaMetres2"] = float(areas[owned & mountain].sum())
        s["coastalAreaMetres2"] = float(areas[owned & coastal].sum())
        s["coastlineMetres"] = float(coast_length[owned].sum())
        s["geometry"] = mapping(
            sh.union_all([shape(regions[j]["geometry"]) for j in s["regionIds"]])
        )
    return dict(
        stage="complete-regional-ownership-v31",
        coveragePolicy="every land cell has one political owner" if full_coverage else "optional partial claims",
        omittedProfiles=omitted,
        regionEvaluations=[
            dict(
                regionId=i,
                evaluatedProfiles=list(suitability),
                mountain=bool(mountain[i]),
                coastal=bool(coastal[i]),
                coastDistanceMetres=float(coast_distance[i]),
                suitability={
                    name: float(values[i]) for name, values in suitability.items()
                },
                owner=int(owner[i]),
                claimProposals=sorted(attempted[i]),
                outcome=(
                    "claimed"
                    if owner[i] >= 0
                    else (
                        "area-budget"
                        if budget_rejected[i]
                        else "unreached-frontier-or-unseeded-island"
                    )
                ),
            )
            for i in range(n)
        ],
        geographyStage=product["stage"],
        seed=product["seed"],
        societies=societies,
        regionOwners=owner.tolist(),
        requestedClaimFraction=claimed_fraction,
        actualClaimFraction=float(claimed.sum() / areas.sum()),
        unclaimedRegionIds=np.flatnonzero(owner < 0).tolist(),
        limitations=[
            "capital candidates only; no generated cities",
            "no supply/water feasibility yet",
            "graph travel is a regional proxy, not validated road routing",
            "no mineral survey; mountain preference is a proxy",
            "coast access is not harbour feasibility; maritime links between islands remain pending",
        ],
        units="metres",
        meshing=False,
    )


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("directory")
    a = ap.parse_args()
    root = Path(a.directory)
    p = json.loads((root / "terrain-world.json").read_text())
    out = generate(p)
    (root / "kingdom-regions.json").write_text(json.dumps(out, indent=2) + "\n")
    print(
        "PASS provisional director",
        len(out["societies"]),
        "societies; claimed fraction",
        out["actualClaimFraction"],
    )
