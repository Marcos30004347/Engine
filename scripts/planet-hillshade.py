#!/usr/bin/env python3
"""Hillshaded crop of the solver's cube field, for judging landforms.

A planet-wide height image cannot answer "are there cliffs and benches" — at
2048 across, a 300 m cell is a pixel and every escarpment is sub-pixel. This
renders a window of one cube face under a low sun, which is what makes a break
in slope read as a break in slope.

The window is chosen automatically as the most rugged patch on the requested
face, so the comparison is not cherry-picked by hand between two runs: both are
asked for their roughest ground and shown at the same place when a window is
given explicitly.

Usage:
  scripts/planet-hillshade.py <export-dir> [output.png] [--face N] [--window X,Y,SIZE]
"""

import pathlib
import struct
import sys
import zlib

import numpy as np


def read_cube_field(path):
    data = path.read_bytes()
    if data[:8] != b"PCUBE001":
        raise ValueError(f"{path}: not a planet cube field")
    side = int(np.frombuffer(data, dtype="<u4", count=1, offset=8)[0])
    radius = float(np.frombuffer(data, dtype="<f8", count=1, offset=12)[0])
    count = side * side * 6
    elevation = np.frombuffer(data, dtype="<f4", count=count, offset=20).astype(np.float64)
    return side, radius, elevation.reshape(6, side, side)


def write_png(path, image):
    """8-bit greyscale PNG, no Pillow dependency."""
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


def most_rugged_window(field, size):
    """Top-left corner of the roughest size x size window on this face."""
    side = field.shape[0]
    step = max(1, size // 4)
    best = None
    best_score = -1.0
    for y in range(0, side - size, step):
        for x in range(0, side - size, step):
            window = field[y:y + size, x:x + size]
            if np.count_nonzero(window > 0.0) < window.size * 0.70:
                continue  # mostly ocean; a shaded sea is not a landform
            score = float(np.std(np.abs(np.diff(window, axis=0))) + np.std(np.abs(np.diff(window, axis=1))))
            if score > best_score:
                best_score = score
                best = (x, y)
    return (best if best is not None else (0, 0)), best_score


def hillshade(window, cell_metres, azimuth_degrees=315.0, altitude_degrees=28.0):
    """Standard hillshade. A low sun is what makes a bench read as a bench."""
    dy, dx = np.gradient(window, cell_metres)
    slope = np.arctan(np.hypot(dx, dy))
    aspect = np.arctan2(-dx, dy)
    azimuth = np.radians(azimuth_degrees)
    altitude = np.radians(altitude_degrees)
    shaded = (
        np.sin(altitude) * np.cos(slope)
        + np.cos(altitude) * np.sin(slope) * np.cos(azimuth - aspect)
    )
    return np.clip(shaded, 0.0, 1.0)


def main():
    arguments = [a for a in sys.argv[1:] if not a.startswith("--")]
    options = [a for a in sys.argv[1:] if a.startswith("--")]
    if not arguments:
        print(__doc__)
        return 1
    source = pathlib.Path(arguments[0])
    field_path = source if source.is_file() else source / "planet-erosion-field.bin"
    if not field_path.exists():
        print(f"no cube field at {field_path}")
        return 1
    output = pathlib.Path(arguments[1]) if len(arguments) > 1 else source / "hillshade.png"

    face = 0
    window = None
    for option in options:
        if option.startswith("--face"):
            face = int(option.split("=", 1)[1] if "=" in option else sys.argv[sys.argv.index(option) + 1])
        if option.startswith("--window"):
            value = option.split("=", 1)[1] if "=" in option else sys.argv[sys.argv.index(option) + 1]
            x, y, size = (int(part) for part in value.split(","))
            window = (x, y, size)

    side, radius, elevation = read_cube_field(field_path)
    size = window[2] if window else min(256, side // 2)
    if window:
        x, y = window[0], window[1]
    else:
        # Search every face, not just one: whole faces of this planet are ocean,
        # and shading the sea says nothing about landforms.
        best_score = -1.0
        x, y = 0, 0
        for candidate in range(6):
            corner, score = most_rugged_window(elevation[candidate], size)
            if score > best_score:
                best_score = score
                face, x, y = candidate, corner[0], corner[1]

    patch = elevation[face][y:y + size, x:x + size]
    # Cube cell size varies about twofold across a face; the face-centre value is
    # close enough for a shading normal.
    cell_metres = (np.pi * 0.5 * radius) / side
    shaded = hillshade(patch, cell_metres)
    write_png(output, (shaded * 255.0).astype(np.uint8))
    print(f"face {face} window {x},{y} size {size} cell {cell_metres:.0f} m")
    print(f"relief in window {patch.min():.0f}..{patch.max():.0f} m")
    print(f"wrote {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
