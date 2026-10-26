"""Terrain-deflected steady wind and twelve monthly regional climate states.

A finite-volume potential-flow approximation, not a 3D weather forecast.
Prescribed maritime forcing, conservative air/water flux and seasonal energy
relaxation produce coherent geographic variations without random biome paint.
"""

import numpy as np
from scipy.sparse import lil_matrix
from scipy.sparse.linalg import spsolve
from terrain_landforms import smooth
from terrain_geography import BIOMES

RECIPES = {
    "temperate": dict(
        meanC=12.0,
        seasonAmplitudeC=7.0,
        oceanRainMm=950.0,
        humidity=0.82,
        pet=800.0,
        latitude=48.0,
    ),
    "humid-tropical": dict(
        meanC=26.0,
        seasonAmplitudeC=1.8,
        oceanRainMm=3200.0,
        humidity=0.94,
        pet=1300.0,
        latitude=8.0,
    ),
    "arid": dict(
        meanC=24.0,
        seasonAmplitudeC=8.0,
        oceanRainMm=180.0,
        humidity=0.30,
        pet=1550.0,
        latitude=28.0,
    ),
}


def solve_wind(
    centres,
    areas,
    heights,
    edges,
    wind=(1.0, 0.0),
    speed=6.0,
    barrier_scale=180.0,
    boundary=None,
):
    """Solve div(K grad(phi))=0 over internal Voronoi regions.

    Coastal graph boundary nodes have a prescribed far-field velocity potential.
    Mountain resistance redirects air; all internal face fluxes balance. This
    depth-averaged approximation omits turbulent wakes and vertical convection.
    """
    centres = np.asarray(centres, float)
    areas = np.asarray(areas, float)
    heights = np.asarray(heights, float)
    wind = np.asarray(wind, float)
    if (
        np.linalg.norm(wind) == 0
        or speed <= 0
        or barrier_scale <= 0
        or np.any(areas <= 0)
    ):
        raise ValueError("invalid wind controls")
    wind /= np.linalg.norm(wind)
    n = len(areas)
    from scipy.spatial import ConvexHull

    if boundary is None:
        hull = ConvexHull(centres)
        on_edge = np.abs(centres @ hull.equations[:, :2].T + hull.equations[:, 2])
        boundary = np.flatnonzero(on_edge.min(axis=1) < 1e-7)
    boundary = np.asarray(boundary, dtype=int)
    prescribed = -(centres @ wind) * speed
    matrix = lil_matrix((n, n))
    conductance = []
    for a, b, length in edges:
        a = int(a)
        b = int(b)
        distance = np.linalg.norm(centres[b] - centres[a])
        k = (length / distance) / (
            1 + (max(heights[a], heights[b]) / barrier_scale) ** 2
        )
        conductance.append(k)
        matrix[a, a] += k
        matrix[b, b] += k
        matrix[a, b] -= k
        matrix[b, a] -= k
    full = matrix.tocsr()
    rhs = np.zeros(n)
    for i in boundary:
        matrix[i, :] = 0
        matrix[i, i] = 1
        rhs[i] = prescribed[i]
    potential = spsolve(matrix.tocsr(), rhs)
    if not np.isfinite(potential).all():
        raise ValueError("wind graph is disconnected")
    flux = np.array(
        [
            k * (potential[int(a)] - potential[int(b)])
            for (a, b, _), k in zip(edges, conductance)
        ]
    )
    divergence = full @ potential
    internal = np.ones(n, bool)
    internal[boundary] = False
    # Least-squares reconstruction of regional velocity from normal face fluxes.
    vectors = np.zeros((n, 2))
    normals = [[] for _ in range(n)]
    values = [[] for _ in range(n)]
    for (a, b, length), q in zip(edges, flux):
        a = int(a)
        b = int(b)
        direction = centres[b] - centres[a]
        direction /= np.linalg.norm(direction)
        for i in (a, b):
            normals[i].append(direction)
            values[i].append(q / length)
    for i in range(n):
        vectors[i] = np.linalg.lstsq(normals[i], values[i], rcond=None)[0]
    return dict(
        potential=potential,
        faceFlux=flux,
        regionalVelocity=vectors,
        boundary=boundary,
        divergence=divergence,
        maximumInternalDivergence=float(np.abs(divergence[internal]).max(initial=0)),
        wind=wind,
        speed=speed,
    )


def advect_climate(centres, areas, heights, edges, flow, recipe, month, coast_distance):
    """Monthly moisture capacity, precipitation removal and air-temperature transport."""
    n = len(areas)
    month_phase = 2 * np.pi * (month - 6) / 12
    sea_temperature = recipe["meanC"] + recipe["seasonAmplitudeC"] * 0.55 * np.cos(
        month_phase
    )
    regional_amp = recipe["seasonAmplitudeC"] * (
        0.55 + 0.45 * (1 - np.exp(-coast_distance / 1800))
    )
    equilibrium = (
        recipe["meanC"] + regional_amp * np.cos(month_phase) - 0.0065 * heights
    )
    outgoing = [[] for _ in range(n)]
    incoming_air = np.zeros(n)
    outgoing_air = np.zeros(n)
    for (a, b, _), flux in zip(edges, flow["faceFlux"]):
        a = int(a)
        b = int(b)
        if flux < 0:
            a, b = b, a
            flux = -flux
        if flux < 1e-10:
            continue
        outgoing[a].append((b, float(flux)))
        incoming_air[b] += flux
        outgoing_air[a] += flux
    boundary_input = np.maximum(0, outgoing_air - incoming_air)
    boundary_output = np.maximum(0, incoming_air - outgoing_air)
    # Air flow in m²/s; column-integrated moisture proxy is normalized to the
    # declared ocean climate forcing after advection, with its multiplier logged.
    moisture = np.zeros(n)
    energy = np.zeros(n)
    airheight = np.zeros(n)
    rain = np.zeros(n)
    temperature = np.zeros(n)
    export = total_input = evaporation = 0.0
    ledger = []
    for i in np.argsort(-flow["potential"], kind="stable"):
        air = incoming_air[i] + boundary_input[i]
        if air < 1e-10:
            raise ValueError("stagnant climate domain")
        received = moisture[i] + boundary_input[i] * recipe["humidity"]
        input_water = boundary_input[i] * recipe["humidity"]
        upstream_h = airheight[i] / air
        upstream_t = (energy[i] + boundary_input[i] * sea_temperature) / air
        dh = heights[i] - upstream_h
        # Saturated uplift cools more slowly; descending air warms dry-adiabatically.
        transported = upstream_t - (0.0055 if dh > 0 else 0.0098) * dh
        exposure = 1 - np.exp(-np.sqrt(areas[i]) / 1500)
        temperature[i] = (1 - exposure) * transported + exposure * equilibrium[i]
        evap = air * 0.012 * recipe["humidity"]
        available = received + evap
        capacity = air * recipe["humidity"] * np.exp(-max(heights[i], 0) / 1800)
        condense = max(0, available - capacity)
        ordinary = (available - condense) * (
            0.07 + 0.04 * (1 - np.exp(-np.sqrt(areas[i]) / 500))
        )
        rain[i] = condense + ordinary
        remaining = available - rain[i]
        outflow = outgoing_air[i] + boundary_output[i]
        for b, q in outgoing[i]:
            fraction = q / outflow
            moisture[b] += remaining * fraction
            energy[b] += temperature[i] * q
            airheight[b] += heights[i] * q
        exported = remaining * boundary_output[i] / outflow
        total_input += input_water
        evaporation += evap
        export += exported
        ledger.append(
            dict(
                region=int(i),
                input=input_water,
                incoming=float(moisture[i]),
                evaporation=float(evap),
                precipitation=float(rain[i]),
                downstream=float(remaining - exported),
                export=float(exported),
            )
        )
    # A small island receives externally generated frontal rain as well as local
    # orographic rain. Prescribing this ocean climate avoids inventing tropical
    # desert solely from a 193m hill. Spatial distribution remains wind-derived.
    per_area = rain / areas
    normalization = recipe["oceanRainMm"] / (np.sum(rain) / np.sum(areas))
    precipitation = per_area * normalization
    balance = dict(
        input=float(total_input * normalization),
        evaporation=float(evaporation * normalization),
        precipitation=float(rain.sum() * normalization),
        export=float(export * normalization),
        residual=float(
            (total_input + evaporation - rain.sum() - export) * normalization
        ),
        units="annual-equivalent mm*m2 for this monthly state",
        forcingNormalization=float(normalization),
    )
    return precipitation, temperature, balance


def simulate(graph, coast_distance, preset):
    if preset not in RECIPES:
        raise ValueError("unknown physical climate recipe")
    _, _, centres, areas, heights, edges = graph
    recipe = RECIPES[preset]
    shared = np.zeros(len(areas))
    for a, b, length in edges:
        shared[int(a)] += length
        shared[int(b)] += length
    boundary = np.flatnonzero(np.array([p.length for p in graph[1]]) - shared > 1.0)
    months = []
    flows = []
    rain = []
    temp = []
    for month in range(12):
        angle = np.deg2rad(20 * np.sin(2 * np.pi * month / 12))
        flow = solve_wind(
            centres,
            areas,
            heights,
            edges,
            (np.cos(angle), np.sin(angle)),
            6 + np.cos(2 * np.pi * month / 12),
            boundary=boundary,
        )
        p, t, budget = advect_climate(
            centres, areas, heights, edges, flow, recipe, month, coast_distance
        )
        rain.append(p)
        temp.append(t)
        flows.append(flow)
        months.append(
            dict(
                month=month + 1,
                precipitationMm=(p / 12).tolist(),
                temperatureC=t.tolist(),
                regionalVelocityMetresPerSecond=flow["regionalVelocity"].tolist(),
                faceFlux=flow["faceFlux"].tolist(),
                potential=flow["potential"].tolist(),
                maximumInternalDivergence=flow["maximumInternalDivergence"],
                waterBudget=budget,
            )
        )
    return (
        np.array(rain).mean(axis=0),
        np.array(temp),
        dict(
            preset=preset,
            parameters=recipe,
            solver="finite-volume steady terrain-resisted potential flow; monthly advective moisture and temperature",
            boundaryCondition="far-field potential at coastal regions; external maritime climate supply",
            stylized=False,
            months=months,
            windIsSteadyApproximation=True,
            weatherForecast=False,
        ),
    )


def raster_climate(graph, h, cell, mask, coast, preset):
    centres = graph[2]
    dist = np.array(
        [coast[int(round(c[1] / cell)), int(round(c[0] / cell))] for c in centres]
    )
    rain, monthly, metadata = simulate(graph, dist, preset)
    x, z = np.meshgrid(np.arange(len(h)) * cell, np.arange(len(h)) * cell)
    total = np.zeros_like(h)
    p = np.zeros_like(h)
    t = np.zeros((12, *h.shape))
    wind = np.zeros((2, *h.shape))
    # Remove regional lapse before interpolation, then apply local elevation.
    # Otherwise a high ridge and a low valley would share a blurred temperature.
    reference = monthly + 0.0065 * graph[4][None, :]
    vectors = np.array(metadata["months"][6]["regionalVelocityMetresPerSecond"])
    for i, c in enumerate(centres):
        w = np.exp(-((x - c[0]) ** 2 + (z - c[1]) ** 2) / (2 * 320**2))
        total += w
        p += w * rain[i]
        t += w[None, :, :] * reference[:, i, None, None]
        wind += w[None, :, :] * vectors[i, :, None, None]
    p /= total
    t = t / total[None, :, :] - 0.0065 * h
    wind /= total[None, :, :]
    target = float(rain @ graph[3])
    p *= target / (p[mask].sum() * cell**2)
    mean = t.mean(axis=0)
    pet = np.maximum(
        150, RECIPES[preset]["pet"] + 22 * (mean - RECIPES[preset]["meanC"])
    )
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
    metadata["annualPrecipitationVolumeMmMetres2"] = target
    metadata["temperatureRangeC"] = [float(t[:, mask].min()), float(t[:, mask].max())]
    metadata["annualRainfallRangeMm"] = [float(p[mask].min()), float(p[mask].max())]
    return (
        dict(
            precipitation=p,
            temperature=mean,
            monthly_temperature=t,
            wind=wind,
            pet=pet,
            aridity=aridity,
            biome_weights=weights,
        ),
        metadata,
    )
