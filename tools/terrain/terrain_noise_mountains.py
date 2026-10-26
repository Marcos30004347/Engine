"""Range-guided ridged gradient noise, independent of watershed cell edges."""

import numpy as np
import shapely as sh
from scipy.interpolate import CubicSpline
from shapely.geometry import LineString
from terrain_landforms import smooth
from terrain_water_realization import ChannelSurface


def gradient_noise(x, z, wavelength, seed):
    """World-coordinate lattice hash; identical samples across tiles and batches."""
    if wavelength <= 0:
        raise ValueError("noise wavelength must be positive")
    x, z = np.broadcast_arrays(
        np.asarray(x, float) / wavelength, np.asarray(z, float) / wavelength
    )
    ix, iz = np.floor(x).astype(np.int64), np.floor(z).astype(np.int64)
    fx, fz = x - ix, z - iz

    def dot(dx, dz):
        # Unsigned wraparound is intentional; never seed a finite raster lattice.
        h = (ix + dx).astype(np.uint64) * np.uint64(0x9E3779B185EBCA87)
        h ^= (iz + dz).astype(np.uint64) * np.uint64(0xC2B2AE3D27D4EB4F)
        h ^= np.uint64(int(seed) % (1 << 64))
        h ^= h >> np.uint64(30)
        h *= np.uint64(0xBF58476D1CE4E5B9)
        h ^= h >> np.uint64(27)
        h *= np.uint64(0x94D049BB133111EB)
        h ^= h >> np.uint64(31)
        angle = (h & np.uint64(65535)).astype(float) * (2 * np.pi / 65536)
        return np.cos(angle) * (fx - dx) + np.sin(angle) * (fz - dz)

    u, v = smooth(fx), smooth(fz)
    return (
        (1 - u) * ((1 - v) * dot(0, 0) + v * dot(0, 1))
        + u * ((1 - v) * dot(1, 0) + v * dot(1, 1))
    ) * 1.4142135623730951


def ridged_field(x, z, seed):
    """Two warp scales followed by five rotated, coarse-gated ridge octaves."""
    wx = (
        x
        + 240 * gradient_noise(x, z, 1200, seed + 1)
        + 75 * gradient_noise(x, z, 430, seed + 2)
    )
    wz = (
        z
        + 240 * gradient_noise(x, z, 1200, seed + 3)
        + 75 * gradient_noise(x, z, 430, seed + 4)
    )
    total = np.zeros_like(wx)
    gate = np.ones_like(wx)
    normalization = 0.0
    for k, wavelength in enumerate([950, 475, 237.5, 118.75, 59.375]):
        a = 0.47 + k * 0.63
        nx, nz = np.cos(a) * wx - np.sin(a) * wz, np.sin(a) * wx + np.cos(a) * wz
        ridge = (
            np.maximum(0, 1 - np.abs(gradient_noise(nx, nz, wavelength, seed + 40 + k)))
            ** 2
        )
        amplitude = 0.48**k
        total += amplitude * ridge * gate
        normalization += amplitude
        gate = np.clip(ridge * 1.7, 0, 1)
    return total / normalization


def noise_mountains(base, cell, weight, coast, land, rivers, seed, range_path):
    """Replace mountain relief, reserve existing drainage, leave plains intact."""
    x, z = np.meshgrid(np.arange(len(base)) * cell, np.arange(len(base)) * cell)
    points = np.column_stack([x.ravel(), z.ravel()])
    controls = np.asarray(range_path, dtype=float)
    if (
        controls.ndim != 2
        or controls.shape[1] != 2
        or len(controls) < 2
        or not np.isfinite(controls).all()
    ):
        raise ValueError("range path needs at least two finite 2D points")
    if np.any(np.linalg.norm(np.diff(controls, axis=0), axis=1) <= 0):
        raise ValueError("range path has duplicate consecutive points")
    arc = np.r_[0, np.cumsum(np.linalg.norm(np.diff(controls, axis=0), axis=1))]
    path = LineString(
        CubicSpline(arc, controls, bc_type="natural")(np.linspace(0, arc[-1], 601))
    )
    distance = sh.distance(sh.points(points), path).reshape(base.shape)
    width = 480 + 85 * gradient_noise(x, z, 850, seed + 20)
    envelope = np.exp(-((distance / width) ** 2))
    ridge = ridged_field(x, z, seed)
    lowland = 8 * (1 - np.exp(-coast / 180)) + 0.012 * coast
    relief = envelope * (100 + 170 * ridge**2.0)
    target = lowland + relief
    channel = ChannelSurface(base, cell, land, rivers)
    dr, bed, rw, rd = channel.project(points)
    dr, bed, rw, rd = (a.reshape(base.shape) for a in (dr, bed, rw, rd))
    # Existing river grades are boundary conditions. Reserve broad valleys, with
    # a smooth lower bound at the banks so replacing the field cannot perch water.
    bank_floor = bed + rd + 2 + 0.12 * dr
    valley_width = 90 + rw * 2 + np.maximum(target - bed - rd, 0) * 1.4
    valley = smooth(dr / valley_width)
    target = target * valley + np.minimum(target, bank_floor) * (1 - valley)
    target = np.maximum(target, (bed + rd + 3) * (1 - smooth((dr - 55) / 130)))
    blend = smooth(weight)
    result = base * (1 - blend) + target * blend
    result[~sh.contains_xy(land, x, z)] = 0
    return (
        result,
        dr,
        dict(
            method="domain-warped ridged gradient noise followed by hydraulic erosion",
            seed=int(seed),
            rangePathMetres=controls.tolist(),
            warpWavelengthsMetres=[1200, 430],
            warpAmplitudesMetres=[240, 75],
            ridgeWavelengthsMetres=[950, 475, 237.5, 118.75, 59.375],
            persistence=0.48,
            rangeWidthMetres=480,
            widthNoiseMetres=85,
            reliefAmplitudeMetres=170,
            substrateReliefMetres=100,
            ridgeExponent=2.0,
            valleyReliefWidthFactor=1.4,
            watershedEdgesShapeMountains=False,
            riverRoutesRetained=True,
            noiseReference="https://www.redblobgames.com/maps/terrain-from-noise/",
            erosionReference="https://github.com/dandrino/terrain-erosion-3-ways",
        ),
    )
