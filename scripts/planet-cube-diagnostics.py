#!/usr/bin/env python3
"""Slope-area diagnostics on the solver's own cube-face domain.

The channel-regime exponent has to be measured here rather than on the exported
equirectangular atlas. The atlas rows narrow to nothing at the poles, so
thousands of texels cover one cube cell and report enormous drainage at
near-zero slope, which drags the high-area end of the curve down: the same
planet measured both ways gave theta -0.50 on the cube domain and -1.12 on the
atlas. Cube faces have near-uniform cell area and no singularity.

Only face-interior neighbours are used. Cross-face steps are exact in the solver
but reimplementing that transform here would be a second copy to keep in sync,
and edge cells are 0.8% of a 512-cell face — statistically irrelevant to a
median-per-bin fit.

Reads planet-erosion-field.bin, written by the headless bake next to the maps.

Usage: scripts/planet-cube-diagnostics.py <export-directory> [output-directory]
"""

import json
import math
import pathlib
import sys

import numpy as np

# 8-neighbour offsets as (dx, dy), matching the solver's kNeighbours order.
NEIGHBOURS = [(0, 1), (1, 1), (1, 0), (1, -1), (0, -1), (-1, -1), (-1, 0), (-1, 1)]

# Channel initiation: the area above which the fluvial regime should hold.
CHANNEL_AREA_SQUARE_METRES = 1.0e6


def read_cube_field(path):
    data = path.read_bytes()
    if data[:8] != b"PCUBE001":
        raise ValueError(f"{path}: not a planet cube field")
    side = int(np.frombuffer(data, dtype="<u4", count=1, offset=8)[0])
    radius = float(np.frombuffer(data, dtype="<f8", count=1, offset=12)[0])
    count = side * side * 6
    elevation = np.frombuffer(data, dtype="<f4", count=count, offset=20).astype(np.float64)
    drainage = np.frombuffer(data, dtype="<f4", count=count, offset=20 + count * 4).astype(np.float64)
    return side, radius, elevation.reshape(6, side, side), drainage.reshape(6, side, side)


def face_directions(side, face):
    """Unit direction of each cell centre on one face, shaped (side, side, 3)."""
    axis = (np.arange(side) + 0.5) / side * 2.0 - 1.0
    s, t = np.meshgrid(axis, axis, indexing="xy")
    ones = np.ones_like(s)
    if face == 0:
        d = np.stack([ones, -t, -s], axis=-1)
    elif face == 1:
        d = np.stack([-ones, -t, s], axis=-1)
    elif face == 2:
        d = np.stack([s, ones, t], axis=-1)
    elif face == 3:
        d = np.stack([s, -ones, -t], axis=-1)
    elif face == 4:
        d = np.stack([s, -t, ones], axis=-1)
    else:
        d = np.stack([-s, -t, -ones], axis=-1)
    return d / np.linalg.norm(d, axis=-1, keepdims=True)


def steepest_descent(side, radius, elevation, face):
    """Steepest downhill gradient per interior cell, in metres per metre."""
    directions = face_directions(side, face)
    field = elevation[face]
    interior = slice(1, side - 1)
    best = np.zeros((side - 2, side - 2))
    centre = field[interior, interior]
    centreDirection = directions[interior, interior]
    for offset_x, offset_y in NEIGHBOURS:
        rows = slice(1 + offset_y, side - 1 + offset_y)
        columns = slice(1 + offset_x, side - 1 + offset_x)
        neighbour = field[rows, columns]
        neighbourDirection = directions[rows, columns]
        distance = np.maximum(np.linalg.norm(centreDirection - neighbourDirection, axis=-1) * radius, 1.0e-3)
        best = np.maximum(best, (centre - neighbour) / distance)
    return best


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    source = pathlib.Path(sys.argv[1])
    field = source if source.is_file() else source / "planet-erosion-field.bin"
    if not field.exists():
        print(f"no cube field at {field}")
        return 1
    output = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else source / "diagnostics"
    output.mkdir(parents=True, exist_ok=True)

    side, radius, elevation, drainage = read_cube_field(field)

    slopes = []
    areas = []
    for face in range(6):
        interior = slice(1, side - 1)
        slope = steepest_descent(side, radius, elevation, face)
        area = drainage[face][interior, interior]
        height = elevation[face][interior, interior]
        keep = (height > 0.0) & (area > 0.0) & (slope > 0.0)
        slopes.append(slope[keep])
        areas.append(area[keep])
    slope = np.concatenate(slopes)
    area = np.concatenate(areas)

    bins = np.logspace(np.log10(area.min()), np.log10(area.max()), 32)
    indices = np.digitize(area, bins)
    table = []
    for index in range(1, len(bins)):
        selected = slope[indices == index]
        if selected.size >= 16:
            table.append((math.sqrt(bins[index - 1] * bins[index]), float(np.median(selected)), int(selected.size)))

    theta = None
    r_squared = None
    if len(table) >= 6:
        values = np.array(table)
        channel = values[values[:, 0] >= CHANNEL_AREA_SQUARE_METRES]
        if len(channel) >= 4:
            log_area = np.log10(channel[:, 0])
            log_slope = np.log10(channel[:, 1])
            fit = np.polyfit(log_area, log_slope, 1)
            theta = float(fit[0])
            residual = log_slope - np.polyval(fit, log_area)
            variance = float(np.sum((log_slope - log_slope.mean()) ** 2))
            r_squared = float(1.0 - np.sum(residual ** 2) / variance) if variance > 0 else None

    if table:
        np.savetxt(output / "cube-slope-area.csv", np.array(table), delimiter=",",
                   header="drainageAreaSquareMetres,medianSlope,sampleCount", comments="")
    land = elevation > 0.0
    summary = {
        "faceSide": side,
        "planetRadiusMetres": radius,
        "landCells": int(np.count_nonzero(land)),
        "reliefMetres": float(elevation.max()),
        "medianLandSlope": float(np.median(slope)),
        "maximumDrainageSquareKilometres": float(drainage.max() / 1.0e6),
        "channelRegimeTheta": theta,
        "channelRegimeRSquared": r_squared,
    }
    (output / "cube-summary.json").write_text(json.dumps(summary, indent=2) + "\n")

    print(f"cube side            {side}")
    print(f"relief               {summary['reliefMetres']:.1f} m")
    print(f"median slope         {summary['medianLandSlope']:.4f}")
    print(f"max drainage         {summary['maximumDrainageSquareKilometres']:.1f} km2")
    print(f"channel theta        {theta if theta is None else round(theta, 4)}"
          f"  r2 {r_squared if r_squared is None else round(r_squared, 3)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
