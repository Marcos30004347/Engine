from __future__ import annotations

import torch
from torch import Tensor
import torch.nn.functional as F


def _gradient(value: Tensor) -> tuple[Tensor, Tensor]:
    return value[..., :, 1:] - value[..., :, :-1], value[..., 1:, :] - value[..., :-1, :]


def ssr_loss(
    prediction: Tensor,
    reference: Tensor,
    sparse_radiance: Tensor,
    sample_mask: Tensor,
) -> tuple[Tensor, dict[str, Tensor]]:
    prediction_log = torch.log1p(prediction.clamp_min(0.0))
    reference_log = torch.log1p(reference.clamp_min(0.0))
    charbonnier = torch.sqrt((prediction_log - reference_log).square() + 1.0e-6).mean()
    pred_dx, pred_dy = _gradient(prediction_log)
    ref_dx, ref_dy = _gradient(reference_log)
    gradient = F.l1_loss(pred_dx, ref_dx) + F.l1_loss(pred_dy, ref_dy)
    sampled_count = sample_mask.sum().clamp_min(1.0)
    consistency = (((prediction - sparse_radiance).abs() * sample_mask).sum() / (3.0 * sampled_count))
    total = charbonnier + 0.2 * gradient + 0.05 * consistency
    return total, {
        "charbonnier": charbonnier.detach(),
        "gradient": gradient.detach(),
        "sampled": consistency.detach(),
    }
