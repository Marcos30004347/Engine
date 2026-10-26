"""Seeded land/ocean classification with emergent islands and embayments.

The sampled 2D field is only used to extract canonical coast polygons. It is
not terrain height, a shading grid, or a meshing product.
"""

import numpy as np
import shapely as sh
import contourpy
from shapely.geometry import Polygon, box
from terrain_noise_mountains import gradient_noise


def natural_coast(seed, extent, land_fraction=0.37):
    if not 0.2 <= land_fraction <= 0.5:
        raise ValueError("land fraction must be between 0.2 and 0.5")
    side = max(129, int(np.ceil(extent / 60)) + 1)
    axis = np.linspace(0, extent, side)
    x, z = np.meshgrid(axis, axis)
    # Broad independent displacement breaks the noise lattice's alignment.
    dx = 0.13 * extent * gradient_noise(x, z, 23000, seed + 301)
    dz = 0.13 * extent * gradient_noise(x, z, 23000, seed + 302)
    xx, zz = x + dx, z + dz
    field = np.zeros_like(x)
    wavelengths = [26000, 11000, 3800, 1200, 350]
    for i, (weight, wavelength) in enumerate(
        zip([0.9, 0.40, 0.15, 0.06, 0.02], wavelengths)
    ):
        field += weight * gradient_noise(xx, zz, wavelength, seed + 310 + i)
    xn, zn = (xx / extent - 0.5) * 2, (zz / extent - 0.5) * 2
    # A weak continental preference groups land without prescribing its outline.
    field -= 0.48 * ((xn / 0.88) ** 2 + (zn / 1.1) ** 2)
    edge = np.minimum.reduce([x, z, extent - x, extent - z])
    field -= 2 * np.clip(1 - edge / 7000, 0, 1) ** 2
    threshold = float(np.quantile(field, 1 - land_fraction))
    # Explicit ocean rim keeps every extracted outer contour closed.
    field[[0, -1], :] = min(field.min(), threshold - 1)
    field[:, [0, -1]] = min(field.min(), threshold - 1)
    contours = contourpy.contour_generator(x=axis, y=axis, z=field).lines(threshold)
    candidates = []
    for ring in contours:
        if len(ring) >= 4 and np.allclose(ring[0], ring[-1]):
            p = Polygon(ring)
            if p.is_valid and p.area >= 2_000_000:
                candidates.append(p)
    # Nested rings are filled here: this stage chooses ocean coasts, not lakes.
    land = sh.union_all(candidates)
    # Physical feature filter rounds needle tips and removes very narrow necks.
    # It never adds fixed bays, circular satellites or a target island count.
    land = land.buffer(-100, quad_segs=12).buffer(100, quad_segs=12)
    parts = [
        Polygon(p.exterior).simplify(8, preserve_topology=True)
        for p in sh.get_parts(land)
        if p.area >= 2_000_000
    ]
    land = sh.union_all(parts)
    if land.is_empty or not land.is_valid or not box(0, 0, extent, extent).covers(land):
        raise ValueError("invalid natural coastline")
    return land, dict(
        method="domain-warped multiscale land field, quantile land budget, vector coast extraction",
        islandCount=len(sh.get_parts(land)),
        targetLandFraction=land_fraction,
        actualLandFraction=float(land.area / extent**2),
        fieldThreshold=threshold,
        fieldSampleMetres=extent / (side - 1),
        fieldPurpose="temporary 2D land/ocean selection only; not height or meshing",
        noiseWavelengthsMetres=wavelengths,
        minimumIslandAreaMetres2=2_000_000,
        minimumFeatureFilterMetres=100,
        fixedIslandLocations=False,
        fixedBayLocations=False,
        closedInteriorContours="filled as land; lake placement deferred",
    )
