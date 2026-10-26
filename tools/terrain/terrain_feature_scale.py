"""Physical feature dimensions; world extent controls composition, never these units."""

from dataclasses import dataclass, asdict
import numpy as np
from terrain_noise_mountains import gradient_noise
from terrain_landforms import smooth


@dataclass(frozen=True)
class DuneScale:
    wavelength_metres: float = 40.0
    minimum_height_metres: float = 0.35
    maximum_height_metres: float = 2.0
    stoss_fraction: float = 0.82
    warp_wavelength_metres: float = 180.0
    warp_amplitude_metres: float = 16.0
    patch_wavelength_metres: float = 180.0

    def __post_init__(self):
        values = list(asdict(self).values())
        if not np.isfinite(values).all() or not (
            self.wavelength_metres > 0
            and 0 <= self.minimum_height_metres <= self.maximum_height_metres
            and 0.5 < self.stoss_fraction < 1
            and self.warp_wavelength_metres > 0
            and self.warp_amplitude_metres >= 0
            and self.patch_wavelength_metres > 0
        ):
            raise ValueError("invalid physical dune dimensions")


SMALL_DUNES = DuneScale()
EROSION_REFERENCE_CELL_METRES = 6000 / 1024


def dune_surface(x, z, wind, seed, scale=SMALL_DUNES):
    """World-coordinate sand thickness; small transverse dunes, not sand transport.

    Independent of output extent/resolution/tile origin. Wind points downwind.
    Shorter, bounded warps avoid the old kilometre-long sweeping sand ridges.
    No extra high-frequency octave is folded into this height representation.
    """
    v = np.asarray(wind, dtype=float)
    if v.shape != (2,) or not np.isfinite(v).all() or np.linalg.norm(v) < 1e-8:
        raise ValueError("dune wind must be a finite nonzero 2D vector")
    v = v / np.linalg.norm(v)
    warp = scale.warp_amplitude_metres * gradient_noise(
        x, z, scale.warp_wavelength_metres, seed + 901
    )
    phase = np.mod((x * v[0] + z * v[1] + warp) / scale.wavelength_metres, 1)
    profile = np.where(
        phase < scale.stoss_fraction,
        phase / scale.stoss_fraction,
        (1 - phase) / (1 - scale.stoss_fraction),
    )
    supply = smooth((gradient_noise(x, z, 780, seed + 903) + 0.35) / 0.7)
    patch = smooth(
        (gradient_noise(x, z, scale.patch_wavelength_metres, seed + 905) + 0.12) / 0.5
    )
    amplitude = (
        scale.minimum_height_metres
        + (scale.maximum_height_metres - scale.minimum_height_metres) * supply
    )
    return amplitude * patch * profile


def dune_raster(x, z, cell, wind, seed, scale=SMALL_DUNES):
    """Area-average detail using midpoint quadrature; never enlarge waves for LOD.

    At very coarse resolutions omit this relief instead of aliasing it. The
    analytic field remains available for regional refinement. Eligibility masks
    are applied by the caller after filtering so protected areas stay untouched.
    """
    if not np.isfinite(cell) or cell <= 0:
        raise ValueError("cell spacing must be positive metres")
    shape = np.broadcast_shapes(np.shape(x), np.shape(z))
    if cell > scale.wavelength_metres / 4:
        return np.zeros(shape)
    n = max(1, int(np.ceil(cell / 2)))
    result = np.zeros(shape)
    for dz in ((np.arange(n) + 0.5) / n - 0.5) * cell:
        for dx in ((np.arange(n) + 0.5) / n - 0.5) * cell:
            result += dune_surface(x + dx, z + dz, wind, seed, scale)
    return result / n**2


def scale_contract(cell):
    return dict(
        horizontalUnit="metre",
        verticalUnit="metre",
        areaUnit="square metre",
        localDimensionsScaleWithExtent=False,
        compositionControls=[
            "continental outline",
            "planning-site distribution",
            "uplift-site placement",
        ],
        dune=asdict(SMALL_DUNES),
        duneRepresentation=(
            "area-averaged height"
            if cell <= 10
            else "omitted at coarse resolution; analytic field retained"
        ),
        duneSamplesPerWavelength=SMALL_DUNES.wavelength_metres / cell,
        erosionReferenceCellMetres=EROSION_REFERENCE_CELL_METRES,
        erosionResolutionQualified=bool(
            np.isclose(cell, EROSION_REFERENCE_CELL_METRES)
        ),
        erosionLimitation="particle step and brush use raster cells; alternate cell sizes remain experimental",
        geographicQualification="small low-relief island scenario; not a validated regional geology simulation",
    )
