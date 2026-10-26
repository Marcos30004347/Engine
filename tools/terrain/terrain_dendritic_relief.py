"""Biome-conditioned surface detail and drainage-driven valley incision.

Landscape design erosion proxy; exported excavation is separate from particle
sediment transport. Climate is sampled provisionally then solved again afterward.
"""

import numpy as np
from scipy.ndimage import gaussian_filter
from terrain_island_drainage import route, accumulate
from terrain_landforms import smooth
from terrain_noise_mountains import gradient_noise


def biome_detail(height, weights, coast, cell, seed, protected):
    x, z = np.meshgrid(np.arange(len(height)) * cell, np.arange(len(height)) * cell)
    noise = lambda scale, offset: gradient_noise(x, z, scale, seed + offset)
    # Features below two sample cells cannot be represented safely here.
    broad = noise(1700, 801)
    medium = noise(650, 802)
    fine = noise(max(240, 4 * cell), 803)
    desert = 7 * broad + 3 * medium + 1.5 * fine
    scrub = 14 * broad + 8 * medium + 3 * fine
    grass = 5 * broad + 2 * medium + fine
    wood = 18 * broad + 9 * medium + 3 * fine
    wet = 22 * broad + 12 * medium + 4 * fine
    alpine = 30 * (0.5 - np.abs(medium)) + 12 * (0.5 - np.abs(fine))
    delta = np.sum(
        weights * np.stack([desert, scrub, grass, wood, wet, alpine]), axis=0
    )
    delta *= smooth(coast / 700) * smooth(height / 100)
    delta[protected] = 0
    return height + delta, dict(
        model="blended climate-weighted 1700/650/240+ m noise recipes; no cell-edge steps",
        maximumChangeMetres=float(np.abs(delta).max()),
        minimumResolvedWavelengthMetres=max(240, 4 * cell),
        dunes="small dunes deferred to finer local sampling; no island-scale giant dunes",
    )


def incise_valleys(height, mask, coast, cell, rain, protected, passes=5):
    h = height.copy()
    ledgers = []
    # Moisture changes erodibility without requiring final biomes in advance.
    wet = np.clip(rain / 1400, 0.25, 2)
    for iteration in range(passes):
        _, parent, rank, order = route(h, mask, "distributed")
        area = accumulate(mask.astype(float) * cell**2, parent, order)
        flat = h.ravel()
        next_h = flat.copy()
        p = parent.ravel()
        alpha = 0.40 * np.maximum(area.ravel() / cell**2, 1) ** 0.45 * wet.ravel()
        for i in order:
            j = p[i]
            if j < 0 or not mask.ravel()[i]:
                continue
            candidate = (flat[i] + alpha[i] * next_h[j]) / (1 + alpha[i])
            next_h[i] = max(flat[i] * 0.74, min(flat[i], candidate))
        cut = np.maximum(0, h - next_h.reshape(h.shape))
        # Open valley sides as well as a one-cell trench; do not blur the heights.
        cut = gaussian_filter(cut, 0.85)
        cut *= smooth(coast / 450) * smooth(h / 80) * mask
        cut[protected] = 0
        h -= cut
        ledgers.append(
            dict(
                passIndex=iteration,
                excavatedCubicMetres=float(cut.sum() * cell**2),
                maximumCutMetres=float(cut.max()),
            )
        )
    return h, dict(
        model="five implicit drainage-area incision passes with valley-side excavation; not calibrated sediment transport",
        passes=ledgers,
        totalExcavatedCubicMetres=float((height - h).sum() * cell**2),
        massDestination="removed material exported from this landscape-design stage; no deposition simulation",
        maximumCutMetres=float((height - h).max()),
    )
