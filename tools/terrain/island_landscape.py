"""Admission gate for the social map solvers.

The old Gaussian spine/uplift implementation was retired: it changed elevations
AFTER hydrology and was not the user's SIGGRAPH 2013 reference. Generate and
validate terrain with build_hydrology_terrain.py first. A numerical terrain pass
alone does not authorize the unresolved basin/river-to-settlement handoff.
"""

from build_hydrology_terrain import METHOD


def prepare_landscape(foundation, *, preview=False):
    acceptance = foundation.get("terrainAcceptance", {})
    preview_ready = (
        preview
        and foundation.get("landscapeVersion") == 5
        and acceptance.get("mapPreviewReady") is True
        and all(
            acceptance.get("previewChecks", {}).get(k) is True
            for k in (
                "sourceHashVerified",
                "scalarSourceVerified",
                "basinReservationsComplete",
                "riversPreserved",
            )
        )
    )
    if (
        foundation.get("terrainMethod") != METHOD
        or foundation.get("landscapeVersion") not in (4, 5)
        or not acceptance.get("numericalPass")
        or not (acceptance.get("settlementReady") or preview_ready)
        or not acceptance.get("checks")
        or not all(acceptance["checks"].values())
    ):
        raise ValueError(
            "Terrain must pass the SIGGRAPH watershed/erosion stage and basin/river "
            "handoff before roads, cities or farmland can run. Use "
            "build_hydrology_terrain.py; the Gaussian uplift path is retired."
        )
    return foundation
