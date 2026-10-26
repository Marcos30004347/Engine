# Planet generator — next session handoff

Written 2026-08-17 at the end of a long session. Everything below is measured or
verified unless it says otherwise.

---

## The ask

**Port the adaptive CBT into `PolygonTerrainSample` so the globe preview
subdivides as you zoom**, the way `SparseVoxelRendererSample` already does.

Today that sample draws a fixed dual mesh from the Voronoi graph (204k regions →
~1.2M faces, roughly 600 m features at R=100 km). Zoom works now, but past that
mesh density the surface just goes flat because there is no more geometry.

### The design decision to make first

The CBT displaces its triangles by sampling `SparseVoxelPlanetSurfaceAtlas`,
which is built from a `PlanetPackageReader`. `PolygonTerrainSample` currently
previews straight off the in-memory graph and only writes a package on Save.

So either:
- **(a)** build the surface atlas in memory after generation (no package on
  disk), or
- **(b)** require a package write before the globe can subdivide.

(a) keeps the current workflow but needs `loadPlanetOverviewMesh` fed from
something other than a reader. Decide this deliberately — it is the crux of the
port, not an implementation detail.

### Pieces to reuse

- `src/voxel/svraster/rendering/SparseVoxelPlanetCbtUpdateChain.hpp` — update
  chain, allocator, topology passes
- `src/voxel/svraster/rendering/SparseVoxelPlanetSurfaceAtlas.hpp` — the atlas
  the CBT displaces from
- `SparseVoxelRendererSample.cpp` around line 11875 — how the chain, atlas and
  overview pass are wired together
- Feed `PlanetCbtUpdateSettingsGPU` from the sample's existing yaw/pitch/zoom

CBT limits at R=100 km: max subdivision depth 24 → **~26 m triangles**; pool
capacity 2^17 = 131,072 bisectors.

---

## Run it

```sh
# Generate (headless, ~7 s at 2048)
./build-release/samples/PolygonTerrainSample --headless --sphere \
  --resolution 2048 --planet-radius 100000 --seed 9127364512 --output /tmp/planet

# Generator GUI — Generate button now runs erosion; map tabs populate immediately
./build-release/samples/PolygonTerrainSample

# Renderer with adaptive CBT (already the default where supported)
./build-release/tests/SparseVoxelRendererSample \
  --planet-package /tmp/planet/planet.planetpkg \
  --disable-terrain-micro-materials
```

Reference planet: **seed 9127364512, R=100 km, 3 km relief**.

---

## Traps that cost real time this session

**`build/` is a week-stale Debug tree.** Everything gets built into
`build-release/`. Several turns were wasted on "nothing changed" reports that
were a `build/samples/PolygonTerrainSample` binary from Aug 10. Check
`date -r` on the binary before debugging behaviour.

**Determinism must be re-checked on every solver or classifier change.** It is
three runs and a hash. It was verified once at phase 1, assumed for three phases,
and had broken by the time anyone looked — 17% of cells differing, up to 1856 m.

```sh
for R in 1 2 3; do ./build-release/samples/PolygonTerrainSample --headless --sphere \
  --resolution 1024 --planet-radius 100000 --seed 9127364512 --output /tmp/d$R >/dev/null; done
shasum -a 256 /tmp/d{1,2,3}/planet.planetpkg   # must match
```

**WGSL uniform alignment silently diverges from C++.** `array<vec4<f32>, N>`
needs 16-byte alignment; `std::array<float, N>` does not. Phase 3 pushed the
scalar block to 88 bytes and the shader began reading `erodibility` at 96 while
C++ wrote at 88 — every rock took another rock's value and the two hardest read
struct padding. There are now `static_assert`s on `sizeof` and `offsetof` in
`PlanetErosionPass.hpp`. **Add one for any new uniform field.**

**Keep raw-RHI binding groups small.** A group with 8 storage buffers silently
dropped bindings 6–7 and one write target, with correct SPIR-V, correct uploads,
distinct buffers and clean validation layers. Packing per-cell fields into one
struct (4 bindings total) fixed it. See `rhi-storage-binding-ceiling` memory.

**Measure θ on the cube domain, never the atlas.** The equirectangular atlas's
polar rows report enormous drainage at near-zero slope: the same field measures
θ = −0.53 on the cube faces and −1.12 on the atlas. Use
`scripts/planet-cube-diagnostics.py` on `planet-erosion-field.bin`.

**Landform detail only reads at the right zoom.** A 210 m stratum outcrops across
~3 cells; at planet scale every bench is sub-pixel. Use
`scripts/planet-hillshade.py` for a windowed view.

**Do not couple preview failures to `state.generated`.** The globe draw is gated
on it, so an atlas failure took the whole planet off screen. Spherical worlds now
survive a failed preview build.

**`planetVoxelTerrainDetail` parameter order**: `..., material, strength,
lithology`. `strength` deliberately stays ahead of `lithology` — inserting a
parameter before an existing defaulted one silently rebound both call sites and
a `double`→`uint8_t` narrowing is legal in a call, so nothing failed to compile.

---

## State of the work

Plan: `docs/planet-generator-geology-erosion-plan.md`.
Results and calibration: `docs/planet-generator-baseline.md`.

| phase | state |
|---|---|
| 0 baseline + diagnostics | done |
| 1 GPU stream power on cube faces | done — θ **−0.532**, r² **0.996**, bit-exact |
| 2 lithology + differential erosion | done — 8 rock types, all exposed |
| 3 regolith/sediment | **implemented, opt-in, uncalibrated** — `--sediment-transport` flattens the planet (θ +0.13); mass balance closes to 7.6% |
| 4 voxel strata | **dropped** — surface-only realism, no terrain editing |
| biomes | done — top-two 55%→25%, 19 of 21 classes, Grass/Rock/Snow/Bare now emitted |

Reference planet now: relief ~1950 m, land 0.40, median slope 0.11, θ −0.532,
19 cover classes, largest 14.9%.

### Open items besides the CBT port

1. **Phase 3 calibration** — `transportK`, `weatheringRate`, `depositionK` need
   their own calibration before fans/valley fill/deltas work. Phase 1's constants
   are not transferable.
2. **Micro-material palette ceiling** — a voxel indexes its brick palette with
   one byte, so >256 distinct micro records per brick fails the build outright.
   Eroded terrain hits it (median slope rose 44×), hence
   `--disable-terrain-micro-materials`. Owner plans a refactor: per-voxel index
   into a per-instance texture atlas.
3. **Surface atlas resolution** is 4096/face (1.15 GB, 18 s generation, 38 m
   texels). Clamp allows 8192. Budget is 5 GB for planet data.

### Architecture note from the owner

The terrain generator must not involve voxels. Voxels read the generated maps
(height, material, lithology) to produce colours and texture indices when
building bricks. Extra runtime detail belongs in the voxel renderer, not baked
per-vertex on the planet mesh.

---

## Verify before declaring anything done

```sh
cd build-release && ctest -R "Planet|SphericalTerrain|PolygonTerrain|VoxelMaterial"
```

20/20 pass at handoff. `SparseVoxelBrickOcclusionEquivalenceG22` fails
independently of this work — verified against stashed changes.

**And actually look at it in the window.** Every number in the baseline doc came
from headless runs. Twice this session a change was reported as working when the
app showed nothing, because headless verification cannot see a gated draw call.
