import torch

from ssr.dataset import blue_noise_block_mask
from ssr.losses import ssr_loss
from ssr.model import NeuralBilateralGridDenoiser


def test_model_shape_gradient_and_finite_output() -> None:
    model = NeuralBilateralGridDenoiser(hidden_channels=8)
    height, width = 24, 32
    sparse = torch.rand(1, 3, height, width)
    mask = (torch.rand(1, 1, height, width) > 0.75).float()
    output = model(
        sparse * mask, mask, torch.randn(1, 3, height, width),
        torch.rand(1, 1, height, width) * 100.0,
        torch.rand(1, 3, height, width), torch.rand(1, 1, height, width),
    )
    assert output.shape == sparse.shape
    assert torch.isfinite(output).all()
    loss, _ = ssr_loss(output, torch.rand_like(output), sparse * mask, mask)
    loss.backward()
    assert any(parameter.grad is not None for parameter in model.parameters())


def test_mask_selects_one_pixel_per_complete_window() -> None:
    mask = blue_noise_block_mask(32, 48, 8, seed=3)
    blocks = mask.view(1, 4, 8, 6, 8).sum(dim=(2, 4))
    assert torch.equal(blocks, torch.ones_like(blocks))
