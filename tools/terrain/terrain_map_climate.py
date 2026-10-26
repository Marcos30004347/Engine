"""Connect the existing conservative wind/climate solvers to noisy map regions."""

import numpy as np
import shapely as sh
from scipy.spatial import cKDTree
from terrain_regional_weather import solve_wind, advect_climate
from terrain_landforms import smooth, sample
from terrain_geography import BIOMES


def simulate_map_climate(product, height, cell, mask):
    regions = product["regions"]
    centres = np.array([r["site"] for r in regions])
    areas = np.array([r["areaMetres2"] for r in regions])
    heights = sample(height, centres, cell)
    edges = [
        (*e["regions"], e["lengthMetres"])
        for e in product["edges"]
        if len(e["regions"]) == 2
    ]
    boundary = sorted(
        {e["regions"][0] for e in product["edges"] if len(e["regions"]) == 1}
    )
    coast = sh.distance(sh.points(centres), sh.geometry.shape(product["land"]).boundary)
    recipe = dict(
        meanC=24.0,
        seasonAmplitudeC=4.0,
        oceanRainMm=1100.0,
        humidity=0.82,
        pet=1300.0,
        latitude=24.0,
    )
    months = []
    rains = []
    temps = []
    winds = []
    for month in range(12):
        phase = 2 * np.pi * month / 12
        angle = np.deg2rad(20 * np.sin(phase))
        wind = solve_wind(
            centres,
            areas,
            heights,
            edges,
            wind=(np.cos(angle), np.sin(angle)),
            speed=6 + np.cos(phase),
            barrier_scale=650,
            boundary=boundary,
        )
        rain, temp, budget = advect_climate(
            centres, areas, heights, edges, wind, recipe, month, coast
        )
        rains.append(rain)
        temps.append(temp)
        winds.append(wind["regionalVelocity"])
        months.append(
            dict(
                month=month + 1,
                temperatureC=temp.tolist(),
                precipitationMm=(rain / 12).tolist(),
                windMetresPerSecond=wind["regionalVelocity"].tolist(),
                faceFlux=wind["faceFlux"].tolist(),
                potential=wind["potential"].tolist(),
                waterBudget=budget,
                maximumInternalDivergence=wind["maximumInternalDivergence"],
            )
        )
    rains = np.array(rains)
    temps = np.array(temps)
    winds = np.array(winds)
    # Four nearby sites on the SAME island interpolate the scalar diagnostics.
    # No fixed 320 m Gaussian kernel, which underflows on this larger map.
    axis = np.arange(len(height)) * cell
    x, z = np.meshgrid(axis, axis)
    points = np.column_stack([x.ravel(), z.ravel()])
    fields = np.zeros((16, len(points)))
    values = np.vstack(
        [
            rains.mean(axis=0),
            temps + heights[None, :] * 0.0065,
            winds.mean(axis=0).T,
            heights,
        ]
    )
    land = sh.geometry.shape(product["land"])
    parts = sorted(sh.get_parts(land), key=lambda p: -p.area)
    for iid, part in enumerate(parts):
        owners = np.array([i for i, r in enumerate(regions) if r["islandId"] == iid])
        at = np.flatnonzero(sh.contains_xy(part, points[:, 0], points[:, 1]))
        tree = cKDTree(centres[owners])
        distance, index = tree.query(points[at], k=min(4, len(owners)))
        if distance.ndim == 1:
            distance = distance[:, None]
            index = index[:, None]
        weights = 1 / np.maximum(distance, 1) ** 2
        weights /= weights.sum(axis=1)[:, None]
        fields[:, at] = np.sum(values[:, owners[index]] * weights[None, :, :], axis=2)
    fields = fields.reshape((16, *height.shape))
    rain = fields[0]
    rain *= float(rains.mean(axis=0) @ areas) / max(rain[mask].sum() * cell**2, 1)
    monthly = fields[1:13] - 0.0065 * height[None, :, :]
    temperature = monthly.mean(axis=0)
    pet = np.maximum(150, recipe["pet"] + 22 * (temperature - recipe["meanC"]))
    aridity = rain / pet
    transitions = [smooth((aridity - v) / 0.24 + 0.5) for v in [0.2, 0.5, 0.9, 1.6]]
    weights = np.stack(
        [
            1 - transitions[0],
            transitions[0] - transitions[1],
            transitions[1] - transitions[2],
            transitions[2] - transitions[3],
            transitions[3],
            np.zeros_like(height),
        ]
    )
    warm = smooth((monthly.min(axis=0) - 16) / 4)
    weights[3] += weights[4] * (1 - warm)
    weights[4] *= warm
    cold = 1 - smooth((temperature - 2) / 5)
    weights[:-1] *= 1 - cold
    weights[-1] = cold
    weights = np.clip(weights, 0, 1)
    weights /= weights.sum(axis=0)
    weights *= mask
    # Smooth annual yield leaves modest wet-season drainage in dry regions.
    # It stays between zero and precipitation; AET is the explicit complement.
    runoff = rain * np.exp(-pet / np.maximum(rain, 1)) * mask
    for i, r in enumerate(regions):
        w = sample(
            weights[0], centres[i : i + 1], cell
        )  # fields below assign exact region values too
        r["climate"] = dict(
            annualRainMm=float(rains[:, i].mean()),
            monthlyTemperatureC=temps[:, i].tolist(),
            monthlyWindMetresPerSecond=winds[:, i].tolist(),
        )
        label = int(
            np.argmax([sample(a, centres[i : i + 1], cell)[0] for a in weights])
        )
        r["biome"] = BIOMES[label]
    return dict(
        precipitation=rain,
        temperature=temperature,
        monthly_temperature=monthly,
        wind=fields[13:15],
        pet=pet,
        aridity=aridity,
        biome_weights=weights,
        runoff_mm=runoff,
    ), dict(
        model="12 steady terrain-resisted regional wind solves with conservative moisture and temperature advection",
        recipe=recipe,
        months=months,
        boundaryRegions=boundary,
        edges=edges,
        windSteadyApproximation=True,
        weatherForecast=False,
        physicallyCalibrated=False,
        annualRainfallRangeMm=[float(rain[mask].min()), float(rain[mask].max())],
        temperatureRangeC=[
            float(monthly[:, mask].min()),
            float(monthly[:, mask].max()),
        ],
        runoffModel="annual proxy P * exp(-PET/P); AET=P-runoff, no storm simulation",
        regionalRainVolumeMmMetres2=float(rains.mean(axis=0) @ areas),
    )
