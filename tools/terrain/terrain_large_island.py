"""Explicit 48 km rugged-island recipe, separate from the 6 km low-island preset."""

import numpy as np
from scipy.ndimage import gaussian_filter
from terrain_fractal_world import make_island, planning_regions, fractal
from terrain_noise_mountains import gradient_noise
from terrain_landforms import smooth


def build_terrain(seed=27, side=1025):
    extent = 48000.0
    x, z, land, mask, coast, cell = make_island(seed, side, extent)
    planning = planning_regions(land, seed, extent, count=180)
    # Explicit physical dimensions for a different geology recipe. These do
    # not scale dunes, beaches or buildings and do not construct a watershed.
    wx = (
        x
        + 750 * gradient_noise(x, z, 6500, seed + 20)
        + 180 * gradient_noise(x, z, 1900, seed + 21)
    )
    wz = (
        z
        + 750 * gradient_noise(x, z, 6500, seed + 22)
        + 180 * gradient_noise(x, z, 1900, seed + 23)
    )
    weight = np.zeros_like(x)
    domains = []
    for k, (cx, cz, major, minor, angle) in enumerate(
        [
            (22500, 13500, 6500, 4800, 0.25),
            (24300, 23500, 7500, 5200, -0.2),
            (23000, 33500, 6000, 4600, 0.3),
            (33000, 28500, 3700, 3400, -0.5),
        ]
    ):
        u = (wx - cx) * np.cos(angle) + (wz - cz) * np.sin(angle)
        v = -(wx - cx) * np.sin(angle) + (wz - cz) * np.cos(angle)
        w = np.exp(-1.5 * ((u / minor) ** 2 + (v / major) ** 2))
        weight = 1 - (1 - weight) * (1 - w)
        domains.append(
            dict(
                id=f"uplift-{k}",
                centre=[cx, cz],
                majorScaleMetres=major,
                minorScaleMetres=minor,
                rotationRadians=angle,
            )
        )
    octaves = max(1, min(6, int(np.floor(np.log2(3200 / (cell * 5)))) + 1))
    ridge = fractal(wx, wz, seed + 100, 3200, octaves, "ridged")
    hills = fractal(x, z, seed + 200, 1800, 3, "fbm")
    h = (
        (28 + 16 * hills + 0.012 * coast + weight * (1600 + 1100 * ridge**1.6))
        * smooth(coast / 950)
        * mask
    )
    return h, weight, land, mask, coast, cell, planning, domains


def climate(h, mask, cell):
    """Seasonal prevailing-wind moisture depletion and leeward recovery.

    A column moisture proxy advected west-to-east; terrain uplift condenses it.
    Units of column supply are annual mm*m; rain removal / cell gives mm/year.
    This explicit climatological scenario is not a calibrated weather forecast.
    All biome weights follow the resulting climate, not painted region labels.
    """
    n = len(h)
    terrain = gaussian_filter(h, max(1, 200 / cell))
    rains = []
    temperatures = []
    budgets = []
    for month in range(12):
        supply = 30e6 * (1 + 0.12 * np.cos(2 * np.pi * month / 12))
        moisture = np.full(n, supply)
        upstream = np.zeros(n)
        rain = np.zeros_like(h)
        injected = n * supply
        exported = evaporation = removed = 0.0
        for col in range(n):
            ocean = ~mask[:, col]
            # Ocean columns replenish the air. The reset is accounted as an
            # exchange with the sea, including moisture leaving land.
            injected += np.maximum(supply - moisture[ocean], 0).sum()
            exported += np.maximum(moisture[ocean] - supply, 0).sum()
            moisture[ocean] = supply
            rise = np.maximum(0, terrain[:, col] - upstream)
            loss = 1 - np.exp(-cell / 20000 - rise / 1200)
            drop = moisture * loss * mask[:, col]
            rain[:, col] = drop / cell
            moisture -= drop
            removed += drop.sum()
            # Slow continental recycling, not instant leeward ocean refill.
            recycle = supply * (1 - np.exp(-cell / 350000)) * mask[:, col]
            moisture += recycle
            evaporation += recycle.sum()
            upstream = terrain[:, col]
        exported += moisture.sum()
        # Unresolved lateral cloud transport spreads precipitation over 800 m.
        # Renormalize after coast clipping to retain the exact moisture removal.
        filtered = gaussian_filter(rain, 800 / cell) * mask
        filtered *= rain.sum() / max(filtered.sum(), 1e-12)
        rains.append(filtered)
        temperatures.append(
            (26 + 2.5 * np.cos(2 * np.pi * (month - 6) / 12) - 0.0065 * h) * mask
        )
        budgets.append(
            dict(
                month=month + 1,
                input=float(injected * cell),
                recycling=float(evaporation * cell),
                precipitation=float(removed * cell),
                export=float(exported * cell),
                residual=float((injected + evaporation - removed - exported) * cell),
                units="annual-equivalent mm*m2",
            )
        )
    p = np.mean(rains, axis=0)
    t = np.array(temperatures)
    mean = t.mean(axis=0)
    pet = np.maximum(300, 1550 + 30 * (mean - 26))
    aridity = p / pet
    transitions = [smooth((aridity - v) / 0.24 + 0.5) for v in (0.2, 0.5, 0.9, 1.6)]
    weights = np.stack(
        [
            1 - transitions[0],
            transitions[0] - transitions[1],
            transitions[1] - transitions[2],
            transitions[2] - transitions[3],
            transitions[3],
            np.zeros_like(h),
        ]
    )
    warm = smooth((t.min(axis=0) - 16) / 4)
    weights[3] += weights[4] * (1 - warm)
    weights[4] *= warm
    cold = 1 - smooth((mean - 2) / 5)
    weights[:-1] *= 1 - cold
    weights[-1] = cold
    weights = np.clip(weights, 0, 1)
    weights /= weights.sum(axis=0)
    weights *= mask
    # Long-term yield with explicit evapotranspiration. No storm-flow claim.
    aet = np.minimum(p, pet * 0.72)
    runoff = (p - aet) * mask
    return dict(
        precipitation=p,
        temperature=mean,
        monthly_temperature=t,
        pet=pet,
        aridity=aridity,
        biome_weights=weights,
        runoff_mm=runoff,
        wind=np.stack([mask * 6.0, mask * 0.0]),
    ), dict(
        model="seasonal ocean-fed prevailing-wind moisture advection and orographic depletion",
        windDirection=[1, 0],
        seaLevelMeanC=26,
        lapseCPerMetre=0.0065,
        monthlyBudgets=budgets,
        weatherForecast=False,
        physicallyCalibrated=False,
        aetRule="min(precipitation, 0.72 * potential evapotranspiration)",
        rainfallRangeMm=[float(p[mask].min()), float(p[mask].max())],
    )
