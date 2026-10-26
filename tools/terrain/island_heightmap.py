"""Export requested scalar heightmap and relief inspection images; no mesh output."""

import json
from pathlib import Path
import numpy as np
import shapely as sh
from shapely.geometry import shape
from scipy.interpolate import LinearNDInterpolator
from scipy.ndimage import gaussian_filter, map_coordinates
from PIL import Image, ImageDraw, ImageFont


def sample_height(product, bounds, size):
    x0, z0, x1, z1 = bounds
    w, h = size
    gx, gz = np.meshgrid(np.linspace(x0, x1, w), np.linspace(z1, z0, h))
    if "terrainRaster" in product:
        raster = product["terrainRaster"]
        source = np.asarray(raster["height"]).reshape(raster["side"], raster["side"])
        heights = map_coordinates(
            source,
            [gz / raster["cellMetres"], gx / raster["cellMetres"]],
            order=1,
            mode="constant",
            cval=0,
        )
    else:
        sites = np.array(product["terrainSites"])
        heights = LinearNDInterpolator(sites[:, :2], sites[:, 2], fill_value=0)(gx, gz)
    mask = sh.contains_xy(shape(product["land"]), gx, gz)
    heights = np.where(mask, np.maximum(0, heights), 0)
    return heights, mask


def relief_rgb(height, mask, metres_per_pixel):
    dz, dx = np.gradient(
        gaussian_filter(height, max(1.0, 12 / metres_per_pixel)), metres_per_pixel
    )
    light = (0.75 - dx * 0.32 + dz * 0.4) / np.sqrt(1 + dx * dx + dz * dz)
    light = np.clip(light, 0.35, 1.15)
    stops = np.array([0, 80, 180, 330, 470, 650, 900])
    colours = np.array(
        [
            [149, 169, 114],
            [128, 151, 96],
            [102, 131, 91],
            [141, 148, 115],
            [159, 151, 135],
            [195, 188, 168],
            [230, 226, 213],
        ]
    )
    rgb = (
        np.stack([np.interp(height, stops, colours[:, i]) for i in range(3)], axis=-1)
        * light[..., None]
    )
    rgb[~mask] = [72, 108, 116]
    return np.uint8(np.clip(rgb, 0, 255))


def export_heightmap(product, directory):
    directory = Path(directory)
    extent = product["extent"]
    size = 513
    height, mask = sample_height(product, (0, 0, extent, extent), (size, size))
    # Heightmap is the natural terrain field. Lake bed/profile contracts remain
    # separate overlays until physical realization; do not imply they are carved.
    np.save(directory / "heightmap-metres.npy", height.astype(np.float32))
    Image.fromarray(np.uint16(np.clip(height * 32, 0, 65535))).save(
        directory / "heightmap-16bit.png"
    )
    rgb = relief_rgb(height, mask, extent / (size - 1))
    im = Image.fromarray(rgb).resize((1539, 1539), Image.Resampling.BICUBIC)
    draw = ImageDraw.Draw(im)
    draw.rectangle((0, 0, 1539, 55), fill="#213e42")
    draw.text(
        (20, 18),
        f"NATURAL HEIGHTMAP | 0–{height.max():.0f} m | ridge uplift + four coherent noise octaves",
        fill="white",
        font=ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 23),
    )
    im.save(directory / "heightmap-relief.png")
    metadata = {
        "kind": "natural-terrain-scalar-map",
        "dimensions": [size, size],
        "boundsMetres": [0, 0, extent, extent],
        "rowOrder": "north-to-south",
        "units": "metres",
        "pngMetresPerInteger": 1 / 32,
        "oceanValue": 0,
        "rangeMetres": [float(height.min()), float(height.max())],
        "lakeBedAndRiverProfilesApplied": False,
        "meshing": False,
    }
    (directory / "heightmap.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(
        f"PASS scalar heightmap: {size} x {size}, maximum {height.max():.2f} m, no mesh"
    )
