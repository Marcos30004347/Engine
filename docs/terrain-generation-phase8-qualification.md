# GPU terrain generation Phase 8: qualification and rollout

Phase 8 adds the authority that decides whether a session may use the GPU
terrain builders. It does not infer safety from API support alone. An exact
backend/device/driver profile must be marked certified and carry every required
suite bit before `auto` can select a GPU mode.

## Runtime modes

Generated terrain now requests `gpu-v2` by default. The default GPU request
fails closed when device qualification, pipeline creation, visual qualification,
or runtime limits are missing; it does not silently generate terrain payloads on
the CPU. `auto` remains the conservative profile-certified rollout mode for
compatibility experiments.

- `auto`: full GPU on a fully certified profile, hybrid GPU on a hybrid-only
  profile, otherwise CPU.
- `cpu`: canonical CPU reference builder. The sample accepts both
  `--terrain-builder cpu` and `--cpu-terrain-builder`.
- `gpu-hybrid`: CPU columns plus GPU raster, Tree64, and joint publication;
  fails closed to CPU when its suites or capabilities are incomplete.
- `gpu-full`: prefer GPU atlas sampling and columns plus the hybrid stages;
  falls back to the qualified hybrid GPU path when only its atlas/column gates
  are unavailable, then to CPU when no GPU terrain path qualifies.

The reusable default, selection, and session policy live in
`TerrainGPUQualification.hpp`. The sample CLI records the requested rollout
mode; the production streaming owner supplies the certified device record and
uses the returned selected mode.

## Qualification contract

Hybrid certification requires raster, Tree64, publication, streaming,
lighting/image, and performance suites. Full certification additionally
requires GPU columns and atlas sampling. Capability checks cover compute,
timestamp queries, workgroup size, storage binding count, storage-buffer size,
and device-local memory.

Backend/tier profiles keep the 750 microsecond target, 1250 microsecond hard
admission ceiling, and 250 microsecond p95 quantum gate invariant. Minimum,
integrated, discrete, and Metal profiles tune dispatch concurrency, page
starts, upload/scratch capacity, work quantum sizes, and cold-estimate scale.

The performance evaluator requires static, walk, fast-travel, and teleport
samples and enforces:

- at least 99 percent of terrain frames at or below 1250 microseconds;
- any overshoot bounded to one quantum;
- quantum p95 at or below 250 microseconds;
- static source-frame p95 regression at or below 300 microseconds;
- no walk source-frame p95 regression;
- at least 30 percent walk request-to-visible improvement at p50 and p95;
- bounded upload/scratch memory and correct publication/lighting on every
  recorded sample.

## Session fallback

Three consecutive non-stale build failures or five consecutive budget
violations disable GPU terrain for the session. A correctness violation or
device loss disables it immediately. Disabling cancels unfinished scheduler
jobs and emits a deduplicated CPU retry for every exact generation key; it does
not mutate terrain residency, slot ownership, or the currently visible page.
This lets the ordinary joint publication transaction replace failed work
without a renderer restart.

The scheduler reports per-stage p95, quantum violations, ceiling violations,
and backend-qualified cold estimates. Immediate timestamp completions and
asynchronously reported timestamps share the same counters.

## Test commands

Configure and build the CPU qualification suite:

```sh
cmake -S . -B build
cmake --build build --target \
  SparseVoxelTerrainPhase0Tests \
  SparseVoxelTerrainPhase1Tests \
  SparseVoxelTerrainPhase2Tests \
  SparseVoxelTerrainPhase3Tests \
  SparseVoxelTerrainPhase4Tests \
  SparseVoxelTerrainPhase5Tests \
  SparseVoxelTerrainPhase6Tests \
  SparseVoxelTerrainPhase7Tests \
  SparseVoxelTerrainPhase8Tests
ctest --test-dir build --output-on-failure -L cpu \
  -R '^SparseVoxelTerrainPhase[0-8]'
```

Build shaders and GPU differential executables:

```sh
cmake --build build --target SparseVoxelRasterShaders \
  SparseVoxelTerrainPhase2GPUTests \
  SparseVoxelTerrainPhase3GPUTests \
  SparseVoxelTerrainPhase4GPUTests \
  SparseVoxelTerrainPhase7GPUTests
```

Run GPU-labelled tests only on a responsive qualification device:

```sh
ctest --test-dir build --output-on-failure -L gpu \
  -R '^SparseVoxelTerrainPhase[2-7]'
```

Certification must also attach captured static, walk, fast-travel, and
teleport results from the minimum supported device and at least one discrete
GPU/backend. A compile-only or SPIR-V validation pass never sets
`deviceProfileCertified`.

## Current qualification result

On 2026-08-27, the CPU Phase 0-8 suites and all terrain shader compilation and
SPIR-V validation gates pass. GPU executables build, but the local Vulkan/Metal
device also hangs on unchanged baseline Phase 3/4 GPU tests. Phase 8 therefore
keeps this profile uncertified and `auto` selects the CPU reference path until
a clean-device run supplies the missing runtime and performance evidence.
