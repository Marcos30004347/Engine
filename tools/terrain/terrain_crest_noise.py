"""Canonical, endpoint-preserving bends and notches along watershed crest edges."""

import numpy as np
import shapely as sh
from shapely.geometry import LineString
from terrain_noise_mountains import gradient_noise


def crest_segments(vertices, edges, seed):
    """Same shared polyline on both flanks; reject newly crossing divide edges."""
    eligible = sorted(
        [e for e in edges if not e["riverCrossing"]], key=lambda e: (e["a"], e["b"])
    )
    reference = [LineString(vertices[[e["a"], e["b"]]][:, [0, 2]]) for e in eligible]
    tree = sh.STRtree(reference)
    accepted = []
    segments = []
    for index, edge in enumerate(eligible):
        a, b = vertices[[edge["a"], edge["b"]]]
        v = b[[0, 2]] - a[[0, 2]]
        length = np.linalg.norm(v)
        if length < 1e-6:
            continue
        normal = np.array([-v[1], v[0]]) / length
        t = np.linspace(0, 1, max(3, int(np.ceil(length / 7)) + 1))
        p = a[None, :] * (1 - t[:, None]) + b[None, :] * t[:, None]
        fade = np.sin(np.pi * t) ** 2
        lateral = (
            32 * gradient_noise(p[:, 0], p[:, 2], 260, seed + 1101)
            + 14 * gradient_noise(p[:, 0], p[:, 2], 95, seed + 1102)
            + 5 * gradient_noise(p[:, 0], p[:, 2], 38, seed + 1103)
        )
        lateral = (
            np.clip(lateral, -min(45, length * 0.10), min(45, length * 0.10)) * fade
        )
        # Height notches break the ruler-straight crest profile without rounding
        # the cross-section. Endpoints retain their canonical junction height.
        vertical = (
            12 * gradient_noise(p[:, 0], p[:, 2], 180, seed + 1104)
            + 4 * gradient_noise(p[:, 0], p[:, 2], 60, seed + 1105)
        ) * fade
        for gain in [1.0, 0.5, 0.25, 0.0]:
            q = p.copy()
            q[:, [0, 2]] += gain * lateral[:, None] * normal
            curve = LineString(q[:, [0, 2]])
            collisions = tree.query(curve)
            if any(
                j != index
                and curve.crosses(reference[j])
                and not reference[index].crosses(reference[j])
                for j in collisions
            ):
                continue
            if any(curve.crosses(other) for other in accepted):
                continue
            break
        q[:, 1] += vertical
        q[0] = a
        q[-1] = b
        accepted.append(LineString(q[:, [0, 2]]))
        segments.extend(np.stack([q[:-1], q[1:]], axis=1))
    segments = np.array(segments)
    return segments[:, 0], segments[:, 1]
