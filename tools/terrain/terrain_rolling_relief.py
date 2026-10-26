"""Continuous gentle relief beneath every landform and on offshore islands."""

import numpy as np
from terrain_noise_mountains import gradient_noise


def lowland_height(xy, coast_distance, seed):
    """Broad rolling terrain in metres; no region-based cutoff or hard flattening."""
    xy = np.asarray(xy, float)
    variation = np.zeros(len(xy))
    for wavelength, weight, offset in [
        (18000, 0.48, 710),
        (7000, 0.28, 711),
        (3500, 0.14, 712),
    ]:
        variation += weight * gradient_noise(
            xy[:, 0], xy[:, 1], wavelength, seed + offset
        )
    # Exponentiation keeps troughs positive without clamping them into flat pans.
    return 85 * np.exp(variation) + 0.003 * np.asarray(coast_distance)


def foothill_height(distance, ridge_width, peak_budget):
    """Low, broad skirt: smooth decay beyond the narrower mountain flank."""
    return min(170, 0.095 * peak_budget) * np.exp(
        -((np.asarray(distance) / (ridge_width * 2.4)) ** 2)
    )
