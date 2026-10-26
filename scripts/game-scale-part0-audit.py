#!/usr/bin/env python3
"""Reproduce the measurements in Part 0 of the game-scale world plan.

The map command decodes the finest pages of one or more engine.world-map
bundles, strips page gutters, and measures elevation relief in every complete
land-centred window. The log command aggregates the per-frame light-field and
terrain-streaming counters emitted by SparseVoxelRendererSample.

Usage:
  scripts/game-scale-part0-audit.py map world-data /tmp/world-game
  scripts/game-scale-part0-audit.py log renderer.log [renderer-2.log ...]
  renderer-command 2>&1 | scripts/game-scale-part0-audit.py log -
"""

import argparse
import json
import pathlib
import re
import sys

import numpy as np


FIELD_LAYOUTS = {
    "R8_UINT": (np.dtype("u1"), 1),
    "R16_UNORM": (np.dtype("<u2"), 1),
    "RG16_FLOAT": (np.dtype("<f2"), 2),
}

HISTORY_PATTERN = re.compile(
    r"Sparse voxel light field history: frame=(?P<frame>\d+) "
    r"attempted=(?P<attempted>\d+) lost=(?P<lost>\d+) "
    r"\((?P<lost_percent>[0-9.]+)%\) faceRejects=(?P<face>[0-9.]+) "
    r"hashRejects=(?P<hash>[0-9.]+) lodTaps=(?P<lod>[0-9.]+)"
)

STREAMING_PATTERN = re.compile(
    r"generation=(?P<generation>\d+).*?resident=(?P<resident>\d+)"
    r".*?reused=(?P<reused>\d+).*?built=(?P<built>\d+)"
    r".*?refined=(?P<refined>\d+).*?coarsened=(?P<coarsened>\d+)"
    r".*?evicted=(?P<evicted>\d+)"
)


def fail(message):
    raise ValueError(message)


def lz4_block_decompress(source, expected_size):
    """Decode the raw LZ4 blocks used by MapExport, without an LZ4 module."""
    output = bytearray()
    cursor = 0
    source_size = len(source)

    def extended_length(length):
        nonlocal cursor
        if length != 15:
            return length
        while cursor < source_size:
            value = source[cursor]
            cursor += 1
            length += value
            if value != 255:
                return length
        fail("truncated LZ4 extended length")

    while cursor < source_size:
        token = source[cursor]
        cursor += 1
        literal_length = extended_length(token >> 4)
        literal_end = cursor + literal_length
        if literal_end > source_size:
            fail("LZ4 literal exceeds the source block")
        output.extend(source[cursor:literal_end])
        cursor = literal_end
        if cursor == source_size:
            break
        if cursor + 2 > source_size:
            fail("truncated LZ4 match offset")
        offset = source[cursor] | (source[cursor + 1] << 8)
        cursor += 2
        if offset == 0 or offset > len(output):
            fail("invalid LZ4 match offset")
        match_length = extended_length(token & 0x0F) + 4
        pattern = bytes(output[-offset:])
        output.extend((pattern * ((match_length + offset - 1) // offset))[:match_length])
        if len(output) > expected_size:
            fail("LZ4 block expands past its declared size")

    if len(output) != expected_size:
        fail(f"LZ4 block decoded to {len(output)} bytes, expected {expected_size}")
    return output


def field_from_manifest(manifest, name):
    for field in manifest.get("fields", []):
        if field.get("name") == name:
            if not field.get("available", False):
                fail(f"field is unavailable: {name}")
            return field
    fail(f"field is missing: {name}")


def read_finest_field(root, manifest, name):
    field = field_from_manifest(manifest, name)
    if field.get("format") not in FIELD_LAYOUTS:
        fail(f"unsupported {name} format: {field.get('format')}")
    dtype, components = FIELD_LAYOUTS[field["format"]]
    width, height = manifest["resolution"]
    pyramid = manifest["pagePyramid"]
    page_size = int(pyramid["interiorSizePixels"])
    gutter = int(pyramid["gutterPixels"])
    finest = int(pyramid["maximumLevel"])
    raster = np.empty((height, width, components), dtype=dtype)
    coverage = np.zeros((height // page_size, width // page_size), dtype=bool)

    for page in field.get("pages", []):
        if int(page["level"]) != finest:
            continue
        path = root / page["file"]
        stored = path.read_bytes()
        if page["compression"] == "lz4":
            payload = lz4_block_decompress(stored, int(page["uncompressedBytes"]))
        elif page["compression"] == "none":
            payload = stored
        else:
            fail(f"unsupported compression for {path}: {page['compression']}")
        page_width, page_height = page["size"]
        expected = int(page_width) * int(page_height) * dtype.itemsize * components
        if len(payload) != expected:
            fail(f"{path}: payload is {len(payload)} bytes, expected {expected}")
        tile = np.frombuffer(payload, dtype=dtype).reshape(
            int(page_height), int(page_width), components
        )
        interior = tile[gutter:gutter + page_size, gutter:gutter + page_size]
        x = int(page["x"])
        y = int(page["y"])
        raster[y * page_size:(y + 1) * page_size,
               x * page_size:(x + 1) * page_size] = interior
        coverage[y, x] = True

    if not np.all(coverage):
        fail(f"{name}: finest page level does not cover the complete raster")
    return field, raster[..., 0] if components == 1 else raster


def sliding_extreme(values, window, axis, maximum):
    """O(n) van Herk sliding min/max along one axis."""
    operation = np.maximum if maximum else np.minimum
    fill = -np.inf if maximum else np.inf
    length = values.shape[axis]
    padded_length = ((length + window - 1) // window) * window
    padding = [(0, 0)] * values.ndim
    padding[axis] = (0, padded_length - length)
    padded = np.moveaxis(np.pad(values, padding, constant_values=fill), axis, -1)
    shape = padded.shape
    blocks = padded.reshape(*shape[:-1], padded_length // window, window)
    prefix = operation.accumulate(blocks, axis=-1).reshape(shape)
    suffix = operation.accumulate(blocks[..., ::-1], axis=-1)[..., ::-1].reshape(shape)
    starts = np.arange(length - window + 1)
    result = operation(suffix[..., starts], prefix[..., starts + window - 1])
    return np.moveaxis(result, -1, axis)


def relief_windows(elevation, land, window_texels):
    high = sliding_extreme(
        sliding_extreme(elevation, window_texels, 1, True),
        window_texels, 0, True)
    low = sliding_extreme(
        sliding_extreme(elevation, window_texels, 1, False),
        window_texels, 0, False)
    centre = window_texels // 2
    centres = land[
        centre:centre + high.shape[0],
        centre:centre + high.shape[1],
    ]
    return (high - low)[centres]


def audit_bundle(path, windows_km):
    root = pathlib.Path(path)
    manifest = json.loads((root / "manifest.json").read_text())
    if manifest.get("schema") != "engine.world-map":
        fail(f"{root}: not an engine.world-map bundle")
    elevation_field, encoded_elevation = read_finest_field(
        root, manifest, "base_elevation")
    _, water_type = read_finest_field(root, manifest, "water_type")
    _, relief_roughness = read_finest_field(root, manifest, "relief_roughness")

    elevation = (
        encoded_elevation.astype(np.float64) / 65535.0
        * float(elevation_field["scale"][0])
        + float(elevation_field["offset"][0])
    )
    land = water_type == 0
    authored_relief = np.abs(relief_roughness[..., 0].astype(np.float64))
    metres_per_texel = float(manifest["extentMetres"]) / int(manifest["resolution"][0])
    windows = []
    for window_km in windows_km:
        texels = max(1, int(round(window_km * 1000.0 / metres_per_texel)))
        if texels > min(elevation.shape):
            fail(f"{root}: {window_km:g} km window exceeds the raster")
        relief = relief_windows(elevation, land, texels)
        if relief.size == 0:
            fail(f"{root}: {window_km:g} km window has no land-centred samples")
        percentiles = np.quantile(relief, (0.10, 0.50, 0.90))
        windows.append({
            "kilometres": window_km,
            "texels": texels,
            "samples": int(relief.size),
            "p10_metres": float(percentiles[0]),
            "p50_metres": float(percentiles[1]),
            "p90_metres": float(percentiles[2]),
        })
    authored = np.quantile(authored_relief, (0.50, 0.90))
    return {
        "path": str(root),
        "seed": int(manifest["seed"]),
        "extent_km": float(manifest["extentMetres"]) / 1000.0,
        "resolution": list(manifest["resolution"]),
        "metres_per_texel": metres_per_texel,
        "land_fraction": float(np.mean(land)),
        "authored_relief_p50_metres": float(authored[0]),
        "authored_relief_p90_metres": float(authored[1]),
        "authored_relief_over_600_fraction": float(np.mean(authored_relief > 600.0)),
        "windows": windows,
    }


def print_map_report(reports):
    print("| bundle | extent | resolution | m/texel | authored relief p50/p90 | >600 m |")
    print("| --- | ---: | ---: | ---: | ---: | ---: |")
    for report in reports:
        width, height = report["resolution"]
        print(
            f"| {report['path']} | {report['extent_km']:.0f} km | "
            f"{width}x{height} | {report['metres_per_texel']:.1f} | "
            f"{report['authored_relief_p50_metres']:.0f}/"
            f"{report['authored_relief_p90_metres']:.0f} m | "
            f"{100.0 * report['authored_relief_over_600_fraction']:.2f}% |"
        )
    print()
    print("| bundle | window | texels | samples | relief p10 | relief p50 | relief p90 |")
    print("| --- | ---: | ---: | ---: | ---: | ---: | ---: |")
    for report in reports:
        for window in report["windows"]:
            print(
                f"| {report['path']} | {window['kilometres']:g} km | "
                f"{window['texels']} | {window['samples']} | "
                f"{window['p10_metres']:.0f} m | {window['p50_metres']:.0f} m | "
                f"{window['p90_metres']:.0f} m |"
            )


def audit_logs(paths):
    reports = []
    for path_value in paths:
        path = pathlib.Path(path_value)
        text = sys.stdin.read() if path_value == "-" else path.read_text(errors="replace")
        history = [match.groupdict() for match in HISTORY_PATTERN.finditer(text)]
        streaming = [match.groupdict() for match in STREAMING_PATTERN.finditer(text)]
        report = {"path": "stdin" if path_value == "-" else str(path),
                  "history_frames": len(history),
                  "streaming_generations": len(streaming)}
        if history:
            attempted = np.array([int(row["attempted"]) for row in history])
            lost = np.array([int(row["lost"]) for row in history])
            report.update({
                "first_frame": int(history[0]["frame"]),
                "last_frame": int(history[-1]["frame"]),
                "attempted_mean": float(np.mean(attempted)),
                "lost_mean_percent": float(np.mean(100.0 * lost / attempted)),
                "lost_max_percent": float(np.max(100.0 * lost / attempted)),
                "face_rejects_mean_percent": float(np.mean(
                    [100.0 * float(row["face"]) for row in history])),
                "hash_rejects_mean_percent": float(np.mean(
                    [100.0 * float(row["hash"]) for row in history])),
                "lod_taps_mean_percent": float(np.mean(
                    [100.0 * float(row["lod"]) for row in history])),
            })
        if streaming:
            for key in ("resident", "built", "refined", "coarsened", "evicted"):
                values = np.array([int(row[key]) for row in streaming])
                report[f"{key}_minimum"] = int(np.min(values))
                report[f"{key}_maximum"] = int(np.max(values))
                report[f"{key}_total"] = int(np.sum(values))
        reports.append(report)
    return reports


def print_log_report(reports):
    print("| log | frames | attempted/frame | lost mean | lost max | face | hash | LOD-only |")
    print("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for report in reports:
        if report["history_frames"] == 0:
            print(f"| {report['path']} | 0 | - | - | - | - | - | - |")
            continue
        print(
            f"| {report['path']} | {report['history_frames']} | "
            f"{report['attempted_mean']:.0f} | {report['lost_mean_percent']:.3f}% | "
            f"{report['lost_max_percent']:.2f}% | "
            f"{report['face_rejects_mean_percent']:.3f}% | "
            f"{report['hash_rejects_mean_percent']:.3f}% | "
            f"{report['lod_taps_mean_percent']:.1f}% |"
        )
    streaming = [report for report in reports if report["streaming_generations"]]
    if streaming:
        print()
        print("| log | generations | resident range | built | refined | coarsened | evicted |")
        print("| --- | ---: | ---: | ---: | ---: | ---: | ---: |")
        for report in streaming:
            print(
                f"| {report['path']} | {report['streaming_generations']} | "
                f"{report['resident_minimum']}..{report['resident_maximum']} | "
                f"{report['built_total']} | {report['refined_total']} | "
                f"{report['coarsened_total']} | {report['evicted_total']} |"
            )


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    map_parser = subparsers.add_parser("map", help="audit world-map bundles")
    map_parser.add_argument("bundles", nargs="+", help="bundle directories")
    map_parser.add_argument(
        "--windows-km", default="5,16",
        help="comma-separated square window sizes (default: 5,16)")
    map_parser.add_argument("--json", action="store_true", help="emit JSON")
    log_parser = subparsers.add_parser("log", help="aggregate renderer logs")
    log_parser.add_argument("logs", nargs="+", help="renderer log files")
    log_parser.add_argument("--json", action="store_true", help="emit JSON")
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    try:
        if arguments.command == "map":
            windows = [float(value) for value in arguments.windows_km.split(",")]
            if not windows or any(not np.isfinite(value) or value <= 0.0 for value in windows):
                fail("window sizes must be finite and positive")
            reports = [audit_bundle(path, windows) for path in arguments.bundles]
            if arguments.json:
                print(json.dumps(reports, indent=2))
            else:
                print_map_report(reports)
        else:
            reports = audit_logs(arguments.logs)
            if arguments.json:
                print(json.dumps(reports, indent=2))
            else:
                print_log_report(reports)
    except (OSError, KeyError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
