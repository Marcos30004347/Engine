from __future__ import annotations

from pathlib import Path
from typing import Mapping

import numpy as np
import torch
from torch import Tensor
from torch.utils.data import Dataset


def _chw(value: np.ndarray, channels: int, name: str) -> Tensor:
    array = np.asarray(value, dtype=np.float32)
    if array.ndim == 2:
        array = array[..., None]
    if array.ndim != 3:
        raise ValueError(f"{name} must be HWC or CHW, got {array.shape}")
    if array.shape[-1] == channels:
        array = np.moveaxis(array, -1, 0)
    if array.shape[0] != channels:
        raise ValueError(f"{name} must have {channels} channels, got {array.shape}")
    return torch.from_numpy(np.ascontiguousarray(array))


def blue_noise_block_mask(
    height: int, width: int, window: int, seed: int
) -> Tensor:
    """Select exactly one rotating, decorrelated pixel per NxN window."""
    yy, xx = np.mgrid[0:height, 0:width]
    bx = xx // window
    by = yy // window
    # Integer hash gives stable per-block offsets without a bundled texture.
    hashed = (
        bx.astype(np.uint32) * np.uint32(0x9E3779B9)
        ^ by.astype(np.uint32) * np.uint32(0x85EBCA6B)
        ^ np.uint32((seed * 0xC2B2AE35) & 0xFFFFFFFF)
    )
    ox = (hashed & np.uint32(window - 1)).astype(np.int64)
    oy = ((hashed >> np.uint32(8)) & np.uint32(window - 1)).astype(np.int64)
    mask = ((xx % window) == ox) & ((yy % window) == oy)
    return torch.from_numpy(mask.astype(np.float32)[None])


class SSRFrameDataset(Dataset[Mapping[str, Tensor]]):
    """Loads independent `.npz` frames and creates random training crops.

    Required keys are ``reference_radiance``, ``normal`` and ``linear_depth``.
    Optional albedo/roughness default to one/zero. If sparse radiance and mask
    are absent, one reference pixel per 2/4/8/16 block is selected locally.
    """

    def __init__(
        self,
        root: str | Path,
        crop_size: int = 256,
        windows: tuple[int, ...] = (2, 4, 8, 16),
        seed: int = 1,
    ) -> None:
        self.files = sorted(Path(root).glob("*.npz"))
        if not self.files:
            raise ValueError(f"no .npz frames found in {root}")
        if any(window < 2 or window & (window - 1) for window in windows):
            raise ValueError("sampling windows must be powers of two >= 2")
        self.crop_size = crop_size
        self.windows = windows
        self.seed = seed

    def __len__(self) -> int:
        return len(self.files)

    def __getitem__(self, index: int) -> Mapping[str, Tensor]:
        rng = np.random.default_rng(self.seed + index)
        with np.load(self.files[index]) as frame:
            reference = _chw(frame["reference_radiance"], 3, "reference_radiance")
            normal = _chw(frame["normal"], 3, "normal")
            depth = _chw(frame["linear_depth"], 1, "linear_depth")
            albedo = _chw(frame["albedo"], 3, "albedo") if "albedo" in frame else torch.ones_like(reference)
            roughness = _chw(frame["roughness"], 1, "roughness") if "roughness" in frame else torch.zeros_like(depth)
            if "sparse_radiance" in frame and "sample_mask" in frame:
                sparse = _chw(frame["sparse_radiance"], 3, "sparse_radiance")
                mask = _chw(frame["sample_mask"], 1, "sample_mask")
            else:
                window = int(rng.choice(self.windows))
                mask = blue_noise_block_mask(reference.shape[1], reference.shape[2], window, self.seed + index)
                sparse = reference * mask

        tensors = [sparse, mask, normal, depth, albedo, roughness, reference]
        height, width = reference.shape[-2:]
        crop_h = min(self.crop_size, height)
        crop_w = min(self.crop_size, width)
        top = int(rng.integers(0, height - crop_h + 1))
        left = int(rng.integers(0, width - crop_w + 1))
        tensors = [value[:, top : top + crop_h, left : left + crop_w] for value in tensors]
        if rng.random() < 0.5:
            tensors = [value.flip(-1) for value in tensors]
        if rng.random() < 0.5:
            tensors = [value.flip(-2) for value in tensors]
        keys = (
            "sparse_radiance", "sample_mask", "normal", "linear_depth",
            "albedo", "roughness", "reference_radiance",
        )
        return dict(zip(keys, tensors))
