#!/usr/bin/env python3
"""Phase-0 diagnostics for a generated planet.

Reads the headless export directory of PolygonTerrainSample and writes the four
diagnostics the erosion refactor is judged against, plus the metrics the biome
work needs a before number for:

  hypsometry.csv       fraction of land area above each elevation
  drainage.csv         histogram of D8 drainage area
  slope-area.csv       median slope per log-binned drainage area
  heightmap.png        the land height field, for eyeballing
  summary.json         scalar metrics (land fraction, relief, biome sharpness)
  plots.svg            hypsometric and slope-area plots, inline for the doc

The slope-area relation is the load-bearing one. A converged stream-power field
shows a clean break between a low-gradient hillslope regime at small drainage
area and a channel regime where slope falls as a power law in area. A height
field that is really noise plus a few ad-hoc erosion passes does not.

Drainage area is computed here rather than read from the export because the
current generator never produces it: routeRainfallRunoff emits a river *mask*,
not accumulated area. Computing it the same way the GPU solver will lets the
two be compared directly.

Usage: scripts/planet-diagnostics.py <export-directory> [output-directory]
"""

import json
import math
import pathlib
import struct
import sys
import zlib

import numpy as np

# D8 neighbour offsets, (dx, dy).
NEIGHBOURS = [(-1, -1), (0, -1), (1, -1), (-1, 0), (1, 0), (-1, 1), (0, 1), (1, 1)]

# PolygonTerrainMaterial, in enum order. Index is the material byte.
MATERIALS = [
    "Ocean", "Lake", "Beach", "Grass", "Rock", "Snow", "River", "Marsh",
    "Ice", "Tundra", "Bare", "Scorched", "Taiga", "Shrubland",
    "TemperateDesert", "TemperateRainForest", "TemperateDeciduousForest",
    "Grassland", "TropicalRainForest", "TropicalSeasonalForest",
    "SubtropicalDesert",
]


def read_pgm(path):
    """Reads a binary PGM (P5), 8 or 16 bit, big-endian as written by the sample."""
    data = path.read_bytes()
    fields = []
    offset = 0
    while len(fields) < 4:
        while offset < len(data) and data[offset : offset + 1].isspace():
            offset += 1
        if data[offset : offset + 1] == b"#":
            while offset < len(data) and data[offset] != 0x0A:
                offset += 1
            continue
        start = offset
        while offset < len(data) and not data[offset : offset + 1].isspace():
            offset += 1
        fields.append(data[start:offset])
    magic, width, height, maximum = fields[0], int(fields[1]), int(fields[2]), int(fields[3])
    if magic != b"P5":
        raise ValueError(f"{path}: not a binary PGM")
    offset += 1
    dtype = np.dtype(">u2") if maximum > 255 else np.uint8
    pixels = np.frombuffer(data, dtype=dtype, count=width * height, offset=offset)
    return pixels.reshape(height, width).astype(np.float64 if maximum > 255 else np.uint8)


def write_png(path, image):
    """Writes an 8-bit greyscale PNG. Avoids a Pillow dependency."""
    height, width = image.shape
    raw = b"".join(b"\x00" + image[row].tobytes() for row in range(height))

    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))

    header = struct.pack(">2I5B", width, height, 8, 0, 0, 0, 0)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def cell_metrics(height, width, radius_metres):
    """Cell width/height in metres per raster row of an equirectangular atlas."""
    latitude = (np.arange(height) + 0.5) / height  # 0 at north pole, 1 at south
    colatitude = latitude * math.pi
    dx = 2.0 * math.pi * radius_metres * np.sin(colatitude) / width
    dy = math.pi * radius_metres / height
    return dx, dy


def shift(field, dx, dy):
    """Neighbour lookup on an equirectangular sphere.

    Longitude wraps. Crossing a pole mirrors the row and advances longitude by
    180 degrees, which is the same rule the erosion shader's atlas_coord uses.
    Clamping instead would build a horizontal wall at each pole.
    """
    height, width = field.shape
    rolled = np.roll(field, -dx, axis=1)
    if dy == 0:
        return rolled
    rows = np.arange(height) + dy
    reflected = rows.copy()
    over = rows < 0
    under = rows > height - 1
    reflected[over] = -rows[over] - 1
    reflected[under] = 2 * height - 1 - rows[under]
    out = rolled[reflected, :]
    flipped = over | under
    if np.any(flipped):
        out[flipped, :] = np.roll(rolled[reflected[flipped], :], width // 2, axis=1)
    return out


def neighbour_table(shape):
    """Flat index of all eight neighbours per cell, with sphere topology."""
    height, width = shape
    flat = np.arange(height * width, dtype=np.int64).reshape(height, width)
    return [shift(flat, offset_x, offset_y).ravel() for offset_x, offset_y in NEIGHBOURS]


def fill_depressions(elevation, land, epsilon=1.0e-4):
    """Priority-flood depression filling, seeded from the ocean.

    Without this, D8 accumulation terminates at every pit: on the current CPU
    height field 5,592 pits swallow a third of the land's drainage, so an
    unfilled field measures the analysis rather than the terrain. Filling adds a
    tiny epsilon gradient so filled flats still route.

    A stream-power field built with z = max(z, z_receiver) has no pits at all,
    which is why the pit count is itself reported as a diagnostic.
    """
    import heapq

    shape = elevation.shape
    neighbours = neighbour_table(shape)
    flat_elevation = elevation.ravel()
    flat_land = land.ravel().astype(bool)
    filled = flat_elevation.copy()
    visited = ~flat_land

    heap = []
    ocean = np.flatnonzero(~flat_land)
    adjacent = np.zeros(flat_land.shape, dtype=bool)
    for table in neighbours:
        adjacent |= flat_land[table]
    for cell in ocean[adjacent[ocean]]:
        heap.append((float(flat_elevation[cell]), int(cell)))
    heapq.heapify(heap)

    while heap:
        level, cell = heapq.heappop(heap)
        for table in neighbours:
            target = int(table[cell])
            if visited[target]:
                continue
            visited[target] = True
            raised = flat_elevation[target] if flat_elevation[target] > level + epsilon else level + epsilon
            filled[target] = raised
            heapq.heappush(heap, (float(raised), target))
    return filled.reshape(shape)


def drainage_and_slope(elevation, land, dx, dy):
    """D8 steepest-descent receivers, drainage area, and downhill slope.

    Returns (area_square_metres, slope, receiver_flat). Slope is the steepest
    descent gradient in metres per metre; ocean cells are outlets and keep zero
    area contribution of their own.
    """
    height, width = elevation.shape
    best_drop = np.zeros_like(elevation)
    best_index = np.full(elevation.shape, -1, dtype=np.int64)
    flat = np.arange(height * width, dtype=np.int64).reshape(height, width)
    dx_grid = np.repeat(dx.reshape(-1, 1), width, axis=1)

    for offset_x, offset_y in NEIGHBOURS:
        neighbour_elevation = shift(elevation, offset_x, offset_y)
        neighbour_index = shift(flat, offset_x, offset_y)
        distance = np.hypot(dx_grid * offset_x, np.full_like(dx_grid, dy * offset_y))
        gradient = (elevation - neighbour_elevation) / distance
        better = gradient > best_drop
        best_drop = np.where(better, gradient, best_drop)
        best_index = np.where(better, neighbour_index, best_index)

    cell_area = (dx_grid * dy) * land
    area = cell_area.copy()
    order = np.argsort(elevation.ravel(), kind="stable")[::-1]
    area_flat = area.ravel()
    receiver_flat = best_index.ravel()
    land_flat = land.ravel().astype(bool)
    # One pass in descending elevation order is a valid topological order for a
    # steepest-descent forest: every receiver is strictly lower than its donor.
    for cell in order:
        if not land_flat[cell]:
            continue
        target = receiver_flat[cell]
        if target >= 0:
            area_flat[target] += area_flat[cell]
    return area_flat.reshape(elevation.shape), best_drop, receiver_flat


def percentile_spread(values):
    return {
        "minimum": float(np.min(values)),
        "median": float(np.median(values)),
        "maximum": float(np.max(values)),
        "mean": float(np.mean(values)),
    }


def svg_plot(series, width, height, title, x_label, y_label, log_x=False, log_y=False):
    """Minimal inline SVG line plot. Themed for both light and dark backgrounds."""
    pad_left, pad_right, pad_top, pad_bottom = 62, 14, 28, 44
    plot_width = width - pad_left - pad_right
    plot_height = height - pad_top - pad_bottom
    xs = np.concatenate([np.asarray(s["x"], dtype=float) for s in series])
    ys = np.concatenate([np.asarray(s["y"], dtype=float) for s in series])
    if log_x:
        xs = np.log10(np.maximum(xs, 1e-12))
    if log_y:
        ys = np.log10(np.maximum(ys, 1e-12))
    x_min, x_max = float(np.min(xs)), float(np.max(xs))
    y_min, y_max = float(np.min(ys)), float(np.max(ys))
    if x_max - x_min < 1e-12:
        x_max = x_min + 1.0
    if y_max - y_min < 1e-12:
        y_max = y_min + 1.0

    def project(x, y):
        if log_x:
            x = math.log10(max(x, 1e-12))
        if log_y:
            y = math.log10(max(y, 1e-12))
        px = pad_left + (x - x_min) / (x_max - x_min) * plot_width
        py = pad_top + plot_height - (y - y_min) / (y_max - y_min) * plot_height
        return px, py

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" '
        f'width="{width}" height="{height}" font-family="ui-sans-serif, system-ui, sans-serif" font-size="11">',
        '<style>'
        '.frame{stroke:currentColor;stroke-opacity:.35;fill:none}'
        '.grid{stroke:currentColor;stroke-opacity:.12}'
        '.label{fill:currentColor;fill-opacity:.75}'
        '.title{fill:currentColor;font-size:12px;font-weight:600}'
        '</style>',
        f'<text class="title" x="{pad_left}" y="16">{title}</text>',
    ]
    for step in range(5):
        y = pad_top + plot_height * step / 4
        value = y_max - (y_max - y_min) * step / 4
        text = f"{10 ** value:.3g}" if log_y else f"{value:.3g}"
        parts.append(f'<line class="grid" x1="{pad_left}" y1="{y:.1f}" x2="{pad_left + plot_width}" y2="{y:.1f}"/>')
        parts.append(f'<text class="label" x="{pad_left - 6}" y="{y + 3:.1f}" text-anchor="end">{text}</text>')
    for step in range(5):
        x = pad_left + plot_width * step / 4
        value = x_min + (x_max - x_min) * step / 4
        text = f"{10 ** value:.3g}" if log_x else f"{value:.3g}"
        parts.append(f'<text class="label" x="{x:.1f}" y="{pad_top + plot_height + 16:.1f}" text-anchor="middle">{text}</text>')
    parts.append(
        f'<rect class="frame" x="{pad_left}" y="{pad_top}" width="{plot_width}" height="{plot_height}"/>'
    )
    for entry in series:
        points = " ".join(
            f"{px:.2f},{py:.2f}"
            for px, py in (project(x, y) for x, y in zip(entry["x"], entry["y"]))
        )
        parts.append(
            f'<polyline fill="none" stroke="{entry["color"]}" stroke-width="1.8" points="{points}"/>'
        )
    parts.append(
        f'<text class="label" x="{pad_left + plot_width / 2:.0f}" y="{height - 8}" text-anchor="middle">{x_label}</text>'
    )
    parts.append(
        f'<text class="label" x="14" y="{pad_top + plot_height / 2:.0f}" text-anchor="middle" '
        f'transform="rotate(-90 14 {pad_top + plot_height / 2:.0f})">{y_label}</text>'
    )
    parts.append("</svg>")
    return "\n".join(parts)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    source = pathlib.Path(sys.argv[1])
    output = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else source / "diagnostics"
    output.mkdir(parents=True, exist_ok=True)

    metadata = json.loads((source / "planet-metadata.json").read_text())
    radius = float(metadata["planetRadiusMetres"])
    height_scale = float(metadata["heightScaleMetres"])

    encoded = read_pgm(source / "heightmap.pgm")
    materials = read_pgm(source / "material-map.pgm")
    rows, columns = encoded.shape
    elevation = encoded * height_scale
    land = (encoded > 0).astype(np.float64)
    land_mask = land.astype(bool)
    dx, dy = cell_metrics(rows, columns, radius)
    dx_grid = np.repeat(dx.reshape(-1, 1), columns, axis=1)
    cell_area = dx_grid * dy

    # Pits are counted on the raw field, then filled: an unfilled field routes
    # a third of this planet's land into depressions and never reaches the sea.
    _, _, raw_receiver = drainage_and_slope(elevation, land, dx, dy)
    pits = land_mask & (raw_receiver.reshape(elevation.shape) < 0)
    filled = fill_depressions(elevation, land)
    area, slope, _ = drainage_and_slope(filled, land, dx, dy)

    land_area = float(np.sum(cell_area[land_mask]))
    total_area = float(np.sum(cell_area))
    land_elevation = elevation[land_mask]

    # Hypsometry: fraction of land area above each elevation, area-weighted so
    # the oversampled polar rows of the atlas cannot bias the curve.
    order = np.argsort(land_elevation)
    sorted_elevation = land_elevation[order]
    sorted_area = cell_area[land_mask][order]
    above = 1.0 - np.cumsum(sorted_area) / land_area
    sample = np.linspace(0, len(sorted_elevation) - 1, min(512, len(sorted_elevation))).astype(int)
    hypsometry = np.stack([sorted_elevation[sample], above[sample]], axis=1)
    np.savetxt(output / "hypsometry.csv", hypsometry, delimiter=",",
               header="elevationMetres,fractionOfLandAreaAbove", comments="")

    # Drainage-area histogram over land, log-binned.
    land_area_values = area[land_mask]
    positive = land_area_values[land_area_values > 0]
    bins = np.logspace(np.log10(positive.min()), np.log10(positive.max()), 48)
    counts, edges = np.histogram(positive, bins=bins)
    centres = np.sqrt(edges[:-1] * edges[1:])
    np.savetxt(output / "drainage.csv", np.stack([centres, counts], axis=1), delimiter=",",
               header="drainageAreaSquareMetres,cellCount", comments="")

    # Slope-area: median slope per log-binned drainage area. Ocean and flat
    # cells are excluded; a zero slope would drag the median to the floor and
    # hide the very break this plot exists to show.
    land_slope = slope[land_mask]
    valid = (land_area_values > 0) & (land_slope > 0)
    slope_area = []
    log_bins = np.logspace(np.log10(land_area_values[valid].min()), np.log10(land_area_values[valid].max()), 32)
    indices = np.digitize(land_area_values[valid], log_bins)
    for index in range(1, len(log_bins)):
        selected = land_slope[valid][indices == index]
        if selected.size >= 16:
            centre = math.sqrt(log_bins[index - 1] * log_bins[index])
            slope_area.append((centre, float(np.median(selected)), int(selected.size)))
    np.savetxt(output / "slope-area.csv", np.array(slope_area), delimiter=",",
               header="drainageAreaSquareMetres,medianSlope,sampleCount", comments="")

    # Channel-regime power law: fit log(slope) = log(k) + theta * log(area)
    # above a fixed 1 km^2 channel-initiation area. The threshold is fixed
    # rather than relative to the field's own maximum so the number stays
    # comparable across height fields with different total drainage.
    #
    # A converged stream-power field gives theta near -0.4 to -0.6 with a high
    # r-squared. A near-zero theta means there is no channel regime, and a
    # positive one means the steepest ground coincides with the largest
    # catchments, which is backwards for a fluvial landscape.
    channel_theta = None
    channel_r_squared = None
    if len(slope_area) >= 6:
        table = np.array(slope_area)
        channel = table[table[:, 0] >= 1.0e6]
        if len(channel) >= 4:
            log_area = np.log10(channel[:, 0])
            log_slope = np.log10(channel[:, 1])
            fit = np.polyfit(log_area, log_slope, 1)
            channel_theta = float(fit[0])
            residual = log_slope - np.polyval(fit, log_area)
            variance = float(np.sum((log_slope - log_slope.mean()) ** 2))
            channel_r_squared = float(1.0 - np.sum(residual ** 2) / variance) if variance > 0 else None

    write_png(output / "heightmap.png",
              np.clip(encoded / max(1.0, encoded.max()) * 255.0, 0, 255).astype(np.uint8))

    # Biome-transition sharpness: share of land cells whose 4-neighbourhood
    # holds a different cover class. Every boundary here is a hard step today,
    # so this is the before number for the transition work.
    boundary = np.zeros(materials.shape, dtype=bool)
    for offset_x, offset_y in [(1, 0), (-1, 0), (0, 1), (0, -1)]:
        boundary |= shift(materials.astype(np.int64), offset_x, offset_y) != materials
    boundary &= land_mask
    material_counts = np.bincount(materials[land_mask].ravel(), minlength=len(MATERIALS))

    profile_path = source / "planet-profile.json"
    summary = {
        "source": str(source),
        "rasterWidth": columns,
        "rasterHeight": rows,
        "planetRadiusMetres": radius,
        "heightScaleMetres": height_scale,
        "landFraction": land_area / total_area,
        "landAreaSquareKilometres": land_area / 1.0e6,
        "reliefMetres": float(land_elevation.max()),
        "medianLandElevationMetres": float(np.median(land_elevation)),
        "slopeMetresPerMetre": percentile_spread(slope[land_mask & (slope > 0)]),
        "maximumDrainageAreaSquareKilometres": float(positive.max() / 1.0e6),
        "channelRegimeTheta": channel_theta,
        "channelRegimeRSquared": channel_r_squared,
        "pitCellCount": int(np.count_nonzero(pits)),
        "pitFractionOfLand": float(np.count_nonzero(pits) / max(1, np.count_nonzero(land_mask))),
        "depressionFillMetres": percentile_spread((filled - elevation)[land_mask]),
        "biomeBoundaryFractionOfLand": float(np.count_nonzero(boundary) / max(1, np.count_nonzero(land_mask))),
        "landCoverCounts": {
            MATERIALS[index]: int(count)
            for index, count in enumerate(material_counts)
            if count > 0 and index < len(MATERIALS)
        },
        "profile": json.loads(profile_path.read_text()) if profile_path.exists() else None,
    }
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")

    table = np.array(slope_area) if slope_area else np.zeros((1, 3))
    plots = "\n".join([
        svg_plot([{ "x": hypsometry[:, 1], "y": hypsometry[:, 0], "color": "#3b82f6" }],
                 460, 300, "Hypsometric curve", "fraction of land area above", "elevation (m)"),
        svg_plot([{ "x": table[:, 0], "y": table[:, 1], "color": "#f97316" }],
                 460, 300, "Slope vs drainage area", "drainage area (m^2)", "median slope",
                 log_x=True, log_y=True),
    ])
    (output / "plots.svg").write_text(plots + "\n")

    print(f"land fraction        {summary['landFraction']:.3f}")
    print(f"relief               {summary['reliefMetres']:.1f} m")
    print(f"median slope         {summary['slopeMetresPerMetre']['median']:.4f}")
    print(f"channel theta        {channel_theta if channel_theta is None else round(channel_theta, 4)}"
          f"  r2 {channel_r_squared if channel_r_squared is None else round(channel_r_squared, 3)}")
    print(f"pits                 {summary['pitCellCount']} ({summary['pitFractionOfLand'] * 100:.2f} % of land)")
    print(f"max drainage         {summary['maximumDrainageAreaSquareKilometres']:.1f} km2")
    print(f"biome boundary share {summary['biomeBoundaryFractionOfLand']:.3f} of land cells")
    print(f"wrote {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
