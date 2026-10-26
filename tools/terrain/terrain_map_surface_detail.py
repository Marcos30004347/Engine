"""Continuous elevation reconstruction and explicit art-directed summit cover."""

import numpy as np
from scipy.ndimage import gaussian_filter
from terrain_landforms import smooth
from terrain_noise_mountains import gradient_noise


def reconstruct_relief(height, coast, cell, seed, protected=None, ridge_strength=1.0):
    # Blend gradients across the kilometre-scale design triangles, before erosion.
    # This is an elevation design operation, not sediment transport.
    mountain = smooth((height - 220) / 500) * smooth(coast / 900)
    if protected is not None:
        mountain *= 1 - gaussian_filter(protected.astype(float), 250 / cell)
        mountain[protected] = 0
    broad = gaussian_filter(height, 420 / cell)
    x, z = np.meshgrid(np.arange(len(height)) * cell, np.arange(len(height)) * cell)
    detail = (
        22 * gradient_noise(x, z, 1200, seed + 710)
        + 9 * gradient_noise(x, z, 480, seed + 711)
        + 3 * gradient_noise(x, z, 210, seed + 712)
    )
    # Warped ridged noise creates connected crests and subsidiary spurs,
    # instead of retaining the rounded Gaussian summit as the final shape.
    wx = x + 650 * gradient_noise(x, z, 3600, seed + 720)
    wz = z + 650 * gradient_noise(x, z, 3600, seed + 721)
    ridge = np.zeros_like(height)
    amplitude = 180.0
    for octave, wavelength in enumerate([2200, 1000, 450]):
        n = gradient_noise(wx, wz, wavelength, seed + 730 + octave)
        ridge += amplitude * (0.55 - np.abs(n))
        amplitude *= 0.48
    # Lowland relief participates in drainage, rather than decorating river lines.
    valley = 12 * gradient_noise(wx, wz, 1700, seed + 740) + 5 * gradient_noise(
        wx, wz, 650, seed + 741
    )
    valley_weight = smooth(coast / 600) * smooth(height / 80)
    if protected is not None:
        valley_weight[protected] = 0
    result = (
        height
        + mountain * (broad - height + detail + ridge_strength * ridge)
        + valley_weight * valley
    )
    return result, dict(
        model="420 m design reconstruction plus warped 2200/1000/450 m sharp ridges, relief noise and drainage-driving lowland relief",
        maximumChangeMetres=float(np.abs(result - height).max()),
        volumeChangeCubicMetres=float((result - height).sum() * cell**2),
        sedimentSimulation=False,
    )


def summit_snow(height, mask, cell, seed, snowline_metres=1120, transition_metres=210):
    x, z = np.meshgrid(np.arange(len(height)) * cell, np.arange(len(height)) * cell)
    dz, dx = np.gradient(height, cell)
    snowline = snowline_metres + 90 * gradient_noise(x, z, 1800, seed + 920)
    coverage = smooth((height - snowline) / transition_metres)
    coverage *= 1 - 0.65 * smooth((np.hypot(dx, dz) - 0.55) / 0.7)
    return coverage * mask
