from __future__ import annotations

from dataclasses import dataclass

import torch
from torch import Tensor, nn
import torch.nn.functional as F


@dataclass(frozen=True)
class DenoiserInputs:
    sparse_radiance: Tensor
    sample_mask: Tensor
    normal: Tensor
    linear_depth: Tensor
    albedo: Tensor
    roughness: Tensor


def _bilateral_grid(
    radiance: Tensor, mask: Tensor, guide: Tensor, spatial_stride: int,
    range_bins: int,
) -> tuple[Tensor, Tensor]:
    """Differentiable mask-normalized trilinear bilateral splat and slice."""
    batch, channels, height, width = radiance.shape
    grid_h = (height - 1 + spatial_stride - 1) // spatial_stride + 2
    grid_w = (width - 1 + spatial_stride - 1) // spatial_stride + 2
    device, dtype = radiance.device, radiance.dtype
    y = torch.arange(height, device=device, dtype=dtype).view(1, 1, height, 1) / spatial_stride
    x = torch.arange(width, device=device, dtype=dtype).view(1, 1, 1, width) / spatial_stride
    y, x = y.expand(batch, 1, height, width), x.expand(batch, 1, height, width)
    z = guide.clamp(0.0, 1.0) * (range_bins - 1)
    y0, x0, z0 = y.floor().long(), x.floor().long(), z.floor().long()
    fy, fx, fz = y - y0, x - x0, z - z0
    cells = range_bins * grid_h * grid_w
    batch_offset = torch.arange(batch, device=device).view(batch, 1, 1, 1) * cells
    flat_sum = radiance.new_zeros(batch * cells, channels)
    flat_weight = radiance.new_zeros(batch * cells, 1)
    corners: list[tuple[Tensor, Tensor]] = []
    for dz in (0, 1):
        zi, wz = (z0 + dz).clamp_max(range_bins - 1), (fz if dz else 1.0 - fz)
        for dy in (0, 1):
            yi, wy = (y0 + dy).clamp_max(grid_h - 1), (fy if dy else 1.0 - fy)
            for dx in (0, 1):
                xi, wx = (x0 + dx).clamp_max(grid_w - 1), (fx if dx else 1.0 - fx)
                weight = wz * wy * wx
                index = batch_offset + (zi * grid_h + yi) * grid_w + xi
                corners.append((index, weight))
                splat_weight = (weight * mask).permute(0, 2, 3, 1).reshape(-1, 1)
                flat_index = index.reshape(-1, 1)
                flat_weight.scatter_add_(0, flat_index, splat_weight)
                flat_sum.scatter_add_(
                    0, flat_index.expand(-1, channels),
                    (radiance * weight * mask).permute(0, 2, 3, 1).reshape(-1, channels),
                )
    grid = flat_sum / flat_weight.clamp_min(1.0e-6)
    sliced = radiance.new_zeros(batch, channels, height, width)
    support = radiance.new_zeros(batch, 1, height, width)
    for index, weight in corners:
        flat_index = index.reshape(-1)
        value = grid[flat_index].view(batch, height, width, channels).permute(0, 3, 1, 2)
        valid = (flat_weight[flat_index] > 1.0e-6).to(dtype).view(batch, height, width, 1).permute(0, 3, 1, 2)
        sliced = sliced + value * weight
        support = support + valid * weight
    return sliced, support.clamp(0.0, 1.0)


class NeuralBilateralGridDenoiser(nn.Module):
    """Stateless, mask-aware neural bilateral-grid denoiser.

    Meng et al.'s original method assumes 1 spp everywhere. Every splat here is
    weighted by the explicit ReSTIR sample mask and normalized by support, so
    missing pixels never enter a grid as zero radiance.
    """

    input_channels = 14

    def __init__(
        self, hidden_channels: int = 20,
        spatial_strides: tuple[int, int, int] = (4, 8, 16),
        range_bins: tuple[int, int, int] = (16, 8, 4),
        sampled_pixel_preservation: float = 0.75,
    ) -> None:
        super().__init__()
        if len(spatial_strides) != 3 or len(range_bins) != 3:
            raise ValueError("the runtime contract uses exactly three grid scales")
        self.spatial_strides = spatial_strides
        self.range_bins = range_bins
        self.sampled_pixel_preservation = sampled_pixel_preservation
        self.guide_net = nn.Sequential(
            nn.Conv2d(self.input_channels, hidden_channels, 5, padding=2),
            nn.LeakyReLU(0.2, inplace=True),
            nn.Conv2d(hidden_channels, 6, 5, padding=2),
        )

    @staticmethod
    def _features(inputs: DenoiserInputs) -> Tensor:
        depth = torch.log1p(inputs.linear_depth.clamp_min(0.0))
        dx = F.pad(depth[..., :, 1:] - depth[..., :, :-1], (0, 1, 0, 0))
        dy = F.pad(depth[..., 1:, :] - depth[..., :-1, :], (0, 0, 0, 1))
        return torch.cat((
            torch.log1p(inputs.sparse_radiance.clamp_min(0.0)), inputs.sample_mask,
            inputs.normal, depth, dx, dy, inputs.albedo, inputs.roughness,
        ), dim=1)

    def forward(
        self, sparse_radiance: Tensor, sample_mask: Tensor, normal: Tensor,
        linear_depth: Tensor, albedo: Tensor, roughness: Tensor,
    ) -> Tensor:
        features = self._features(DenoiserInputs(
            sparse_radiance, sample_mask, normal, linear_depth, albedo, roughness
        ))
        guide_and_weights = self.guide_net(features)
        guides = torch.sigmoid(guide_and_weights[:, :3])
        mixture = torch.softmax(guide_and_weights[:, 3:], dim=1)
        reconstructions, supports = [], []
        for scale, (stride, bins) in enumerate(zip(self.spatial_strides, self.range_bins)):
            reconstructed, support = _bilateral_grid(
                sparse_radiance, sample_mask, guides[:, scale : scale + 1], stride, bins
            )
            reconstructions.append(reconstructed)
            supports.append(support)
        support_stack = torch.cat(supports, dim=1)
        effective = mixture * support_stack
        effective = effective / effective.sum(dim=1, keepdim=True).clamp_min(1.0e-6)
        output = sum(
            reconstruction * effective[:, scale : scale + 1]
            for scale, reconstruction in enumerate(reconstructions)
        )
        preserve = float(self.sampled_pixel_preservation)
        if preserve > 0.0:
            sampled = torch.lerp(output, sparse_radiance, preserve)
            output = torch.where(sample_mask > 0.5, sampled, output)
        return output.clamp_min(0.0)
