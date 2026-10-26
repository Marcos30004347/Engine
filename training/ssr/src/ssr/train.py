from __future__ import annotations

import argparse
import math
from pathlib import Path

import torch
from torch.utils.data import DataLoader

from .dataset import SSRFrameDataset
from .losses import ssr_loss
from .model import NeuralBilateralGridDenoiser


def _device(name: str) -> torch.device:
    if name != "auto":
        return torch.device(name)
    if torch.cuda.is_available():
        return torch.device("cuda")
    if torch.backends.mps.is_available():
        return torch.device("mps")
    return torch.device("cpu")


def main() -> None:
    parser = argparse.ArgumentParser(description="Train the stateless ReSTIR SSR denoiser")
    parser.add_argument("data", type=Path)
    parser.add_argument("--validation-data", type=Path)
    parser.add_argument("--output", type=Path, default=Path("runs/ssr"))
    parser.add_argument("--epochs", type=int, default=40)
    parser.add_argument("--batch-size", type=int, default=4)
    parser.add_argument("--crop-size", type=int, default=256)
    parser.add_argument("--learning-rate", type=float, default=2.0e-4)
    parser.add_argument("--hidden-channels", type=int, default=20)
    parser.add_argument("--device", default="auto")
    parser.add_argument("--workers", type=int, default=0)
    args = parser.parse_args()

    device = _device(args.device)
    dataset = SSRFrameDataset(args.data, crop_size=args.crop_size)
    loader = DataLoader(dataset, batch_size=args.batch_size, shuffle=True, num_workers=args.workers)
    validation_loader = None
    if args.validation_data is not None:
        validation_loader = DataLoader(
            SSRFrameDataset(args.validation_data, crop_size=args.crop_size),
            batch_size=args.batch_size, shuffle=False, num_workers=args.workers,
        )
    model = NeuralBilateralGridDenoiser(hidden_channels=args.hidden_channels).to(device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.learning_rate, weight_decay=1.0e-4)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=max(args.epochs, 1))
    args.output.mkdir(parents=True, exist_ok=True)

    best_validation = float("inf")
    for epoch in range(args.epochs):
        model.train()
        running = 0.0
        for batch in loader:
            batch = {key: value.to(device) for key, value in batch.items()}
            optimizer.zero_grad(set_to_none=True)
            prediction = model(
                batch["sparse_radiance"], batch["sample_mask"], batch["normal"],
                batch["linear_depth"], batch["albedo"], batch["roughness"],
            )
            loss, _ = ssr_loss(
                prediction, batch["reference_radiance"],
                batch["sparse_radiance"], batch["sample_mask"],
            )
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()
            running += float(loss.detach())
        scheduler.step()
        mean_loss = running / max(len(loader), 1)
        validation_loss, psnr = float("nan"), float("nan")
        if validation_loader is not None:
            model.eval()
            validation_total, squared_error, value_count = 0.0, 0.0, 0
            with torch.no_grad():
                for batch in validation_loader:
                    batch = {key: value.to(device) for key, value in batch.items()}
                    prediction = model(
                        batch["sparse_radiance"], batch["sample_mask"], batch["normal"],
                        batch["linear_depth"], batch["albedo"], batch["roughness"],
                    )
                    loss, _ = ssr_loss(
                        prediction, batch["reference_radiance"],
                        batch["sparse_radiance"], batch["sample_mask"],
                    )
                    validation_total += float(loss)
                    error = torch.log1p(prediction) - torch.log1p(batch["reference_radiance"].clamp_min(0.0))
                    squared_error += float(error.square().sum())
                    value_count += error.numel()
            validation_loss = validation_total / max(len(validation_loader), 1)
            psnr = -10.0 * math.log10(max(squared_error / max(value_count, 1), 1.0e-12))
        print(
            f"epoch={epoch + 1:03d} train={mean_loss:.6f} "
            f"validation={validation_loss:.6f} log_psnr={psnr:.3f} device={device}"
        )
        payload = {
            "model": model.state_dict(),
            "config": {"hidden_channels": args.hidden_channels, "spatial_strides": [4, 8, 16], "range_bins": [16, 8, 4]},
            "epoch": epoch + 1,
            "loss": mean_loss,
            "validation_loss": validation_loss,
            "log_psnr": psnr,
        }
        torch.save(payload, args.output / "latest.pt")
        if validation_loader is not None and validation_loss < best_validation:
            best_validation = validation_loss
            torch.save(payload, args.output / "best.pt")


if __name__ == "__main__":
    main()
