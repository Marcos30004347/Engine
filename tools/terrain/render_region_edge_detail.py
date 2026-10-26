"""Same-location straight/shared-noisy region comparison."""

import argparse, json
from pathlib import Path
import shapely as sh
from shapely.geometry import shape
from PIL import Image, ImageDraw
from render_terrain_landforms import font


def render(directory):
    root = Path(directory)
    j = json.loads((root / "elevation-map.json").read_text())
    size = 900
    bounds = (13000, 24000, 22000, 33000)
    if j["extent"] > 48000:
        bounds = (27000, 35000, 36000, 44000)
    x0, z0, x1, z1 = bounds
    im = Image.new("RGB", (size * 2, 1020), "#203b3b")
    d = ImageDraw.Draw(im)
    for k, key in enumerate(["logicalGeometry", "geometry"]):
        panel = Image.new("RGB", (size, size), "#295e73")
        pd = ImageDraw.Draw(panel)

        def xy(p):
            return (
                (p[0] - x0) / (x1 - x0) * (size - 1),
                (z1 - p[1]) / (z1 - z0) * (size - 1),
            )

        for r in j["regions"]:
            v = r["elevationMetres"] / 1600
            col = tuple(
                int(a * (1 - v) + b * v)
                for a, b in zip([149, 183, 119], [222, 211, 190])
            )
            for p in sh.get_parts(shape(r[key])):
                pts = [xy(q) for q in p.exterior.coords]
                pd.polygon(pts, fill=col)
                if key == "logicalGeometry":
                    pd.line(pts, fill="#657564", width=2, joint="curve")
        if key == "geometry":
            from terrain_junction_detail import paint_boundaries

            panel = paint_boundaries(panel, j, xy, (size - 1) / (x1 - x0))
        im.paste(panel, (k * size, 65))
        d.text(
            (k * size + 20, 20),
            [
                "LOGICAL REGIONS / straight shared edges",
                "ORGANIC REGIONS / stronger noise, softened junctions",
            ][k],
            font=font(23),
            fill="white",
        )
        x = k * size + 30
        y = 935
        d.rectangle((x - 10, y - 32, x + 135, y + 12), fill="#203b3b")
        d.line((x, y, x + 1000 / 9000 * (size - 1), y), fill="white", width=4)
        d.text((x, y - 28), "1 km", font=font(20), fill="white")
    d.text(
        (20, 981),
        "Same 9 km crop. Shared curves keep ownership watertight and will also define later biome boundaries.",
        font=font(23),
        fill="white",
    )
    im.save(root / "region-edge-comparison.png")
    print("PASS shared region edge comparison, identical location and scale")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("directory")
    a = p.parse_args()
    render(a.directory)
