# Mask-aware Neural Bilateral Grid for sparse ReSTIR

This project trains the spatial denoiser used after the renderer's ReSTIR
visibility traces. It has no recurrent state, motion vectors, reprojection, or
previous-frame inputs. TAA/TSR remains a separate presentation concern.

The selected model is a clean PyTorch adaptation of Meng et al.'s MIT-licensed
Neural Bilateral Grid design. A two-layer GuideNet predicts three guide images
and three mixture weights. Mask-normalized grids at spatial strides 4, 8 and
16 reconstruct large holes cheaply; absent ReSTIR samples contribute no value
or weight. The paper's temporal preprocessing is intentionally omitted.

## Frame format

Put independent `.npz` frames in one directory. Each frame must contain:

- `reference_radiance`: dense, demodulated RGB lighting, HWC or CHW;
- `normal`: world/view geometry normal RGB;
- `linear_depth`: one-channel positive view depth.

`albedo` and `roughness` are optional. If `sparse_radiance` and `sample_mask`
are absent, training synthesizes one decorrelated sample per 2x2, 4x4, 8x8,
or 16x16 block from the reference. Real renderer captures should include both
so the network learns ReSTIR estimator noise as well as missing pixels.

Debug mode 18 exposes the raw training input directly: RGB is current-frame
sparse ReSTIR lighting and alpha is the exact receiver sample mask. Use
`--dump-frame` for that layer and for modes 14 (full-rate reference), 1
(geometry normal), 8 (linear depth), 0 (albedo), and 3 (roughness), then pack
the raw RGBA32F files:

```sh
ssr-pack-frame --width 960 --height 540 \
  --sparse capture/sparse.rgba32f --reference capture/reference.rgba32f \
  --normal capture/normal.rgba32f --depth capture/depth.rgba32f \
  --albedo capture/albedo.rgba32f --roughness capture/roughness.rgba32f \
  --output frames/terrain_0001.npz
```

## Train and export

```sh
cd training/ssr
python3 -m venv .venv
. .venv/bin/activate
pip install -e '.[test,export]'
ssr-train data/terrain/train --validation-data data/terrain/validation \
  --output runs/terrain
ssr-export runs/terrain/latest.pt --output runs/terrain/ssr_weights.npz
```

Generate a bootstrap dataset directly from deterministic renderer trajectory
snapshots. Arguments after `--` are forwarded unchanged to the renderer:

```sh
ssr-generate-dataset \
  --renderer ../../build-release/tests/SparseVoxelRendererSample \
  --output data/terrain --resolution 960x540 \
  --capture-frames 64,96,128,160,192 \
  -- --world-map ../../world-data --terrain-render-distance-km 16
```

The final trajectory snapshots are assigned to `validation/`; they are never
used as training crops. For final quality evaluation, generate validation with
a distinct world map or camera trajectory as a separate invocation. Bootstrap
inputs are masked from an 8-ray full-rate reference. A later capture phase will
add native sparse ReSTIR inputs so estimator noise is represented as well.

Apple Silicon automatically uses MPS; NVIDIA uses CUDA. The `.npz` and JSON
manifest are the engine weight-packing contract. ONNX is optional and intended
for model inspection, not as a required runtime dependency.
