"""Material/UV loading shared by orthographic and perspective native previews."""
import json
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]


class Materials:
    def ground(self, data, triangle_index):
        weights=data.get("groundWeights", [])
        if triangle_index>=len(weights) or sum(weights[triangle_index][0])<.5:
            return None
        ids=data.get("groundMaterialIds", [11,12,13])
        records=[self.records[str(i)] for i in ids]
        return (np.asarray(weights[triangle_index],dtype=float),
                [self.textures[str(i)] for i in ids],
                [r.get("repeatMetres",[1,1]) for r in records])

    def __init__(self, data):
        self.records = {}
        self.textures = {}
        manifest = data.get("textureManifest")
        if not manifest:
            return
        self.path = ROOT / manifest
        self.records = json.loads(self.path.read_text())["materials"]
        for key, record in self.records.items():
            if "texture" in record:
                with Image.open(self.path.parent / record["texture"]) as image:
                    tex = np.asarray(image.convert("RGB"), dtype=np.float32)
                self.textures[key] = np.clip(tex * record.get("tint", [1, 1, 1]), 0, 255).astype(np.uint8)

    def surface(self, data, triangle_index, fallback):
        ids = data.get("surfaceMaterials", [])
        key = str(ids[triangle_index]) if triangle_index < len(ids) else "0"
        record = self.records.get(key, {})
        uv = None
        if "triangleUVs" in data:
            uv = np.asarray(data["triangleUVs"][triangle_index], dtype=float)
        elif "uvs" in data:
            uv = np.asarray([data["uvs"][i] for i in data["triangles"][triangle_index]], dtype=float)
        if uv is not None:
            uv = uv / np.asarray(record.get("repeatMetres", [1, 1]), dtype=float)
            if record.get("wrap") == "clamp":
                # Keep fitted asset endpoints off the repeating sampler's opposite edge.
                uv = np.clip(uv, 0, 1 - 1e-7)
        return record.get("color", fallback), uv, self.textures.get(key)
