# Programmable voxel material system plan

Status: implementation complete through Phase 8. The final phase includes the
bounded material-field ABI, constant GPU fallbacks, stable logical sample
addresses, shared texture residency and mapping-table resolution,
deterministic planet-region grass, dirt, and colour-variation fields, cached
field-driven geometry, and a bounded four-colour shading palette. Stable
material slots,
normalized base-surface context, programmable shading and geometry IR, volume
CSG, optional contextual fields, versioned assets, hot reload, persistence,
graph controls, material debug views, and material-aware Tree64 ray-consumer
parity are implemented.

## Outcome

Promote the existing material-driven microvoxel implementation into a
first-class voxel material system. A voxel material will be able to:

- retain the terrain generator's deterministic base surface, including slopes;
- modify that surface with flat, bumped, deposited, eroded, or carved
  microstructure;
- select heightfield or bounded-volume microgeometry;
- respond to stable context such as slope, depth below the surface, terrain
  blend, biome data, and canonical world coordinates;
- blend up to four bounded material layers, while retaining the exact
  primary/secondary fast path for materials such as green grass over brown
  soil, with programmable hue, brightness, roughness, and metallic variation;
- update shading without rebuilding resident terrain pages;
- update geometry without rebuilding pages when its required context layout is
  unchanged;
- produce deterministic geometry across frames, pages, clipmap levels, CPU
  reference code, and GPU traversal.

The implementation extends the current exact microvoxel traversal rather than
introducing another geometry path. The existing parent voxel remains the
conservative culling envelope. A material may remove or reshape occupancy
inside that envelope, but it may not create geometry outside it.

## Existing foundation

The renderer already provides most of the low-level mechanics:

- deterministic microgeometry profiles for constant columns, hash columns,
  clumped columns, volume density, and terrain staircases;
- canonical integer parent and microcell coordinates;
- exact one-face and volume traversal during brick visibility;
- parent traversal continuation after a completely carved microvolume;
- a stable exact-to-coverage LOD handoff;
- a material descriptor library and descriptor IDs carried through brick
  palettes;
- a per-hit micro payload consumed by the later material G-buffer pass;
- primary and secondary material colours;
- terrain coverage patches containing four shared corner heights;
- live descriptor uploads for the sample terrain materials.

The main gaps are architectural:

1. Terrain slope reconstruction is encoded as a special `Staircase` profile
   rather than a reusable base-surface input.
2. Terrain context is split between material flags, palette descriptor bits,
   and the parallel micro-surface sidecar.
3. Geometry and shading are limited to fixed profile switches with a few packed
   parameters.
4. Biome presets and live editing are owned by
   `SparseVoxelRendererSample.cpp` rather than material assets and a runtime
   material manager.
5. Geometry and shading share one packed descriptor and one revision even
   though they have different invalidation requirements.

## Core decisions

### Terrain owns the base surface; materials modify it

Slope is not a separate material type. Terrain supplies a base surface patch;
the material evaluates a modifier on top of it:

```text
final retained surface
    = terrain base surface
    + deposition or bumps
    - erosion or carving
```

A flat parent has four equal patch heights. A sloped parent has different
corner heights. Adjacent terrain parents share corner values, so bilinear
sampling remains continuous before the result is quantized to microcells.

This separation prevents a grass material and a rock material from
reconstructing different macro terrain at their boundary. Both see the same
terrain surface; only their bounded microstructure differs.

### Geometry and shading are separate programs

The geometry program executes while visibility is being resolved because it
decides occupancy and exact depth. It is pure, deterministic, bounded, and
restricted to approved operations.

The shading program executes after visibility in the material G-buffer pass.
It may change colour and material properties but cannot change occupancy,
depth, or geometric identity.

### Author one graph, compile two typed programs

The authoring format may be a node graph or declarative asset, but compilation
produces two independent typed programs:

```text
VoxelMaterialAsset
    GeometryGraph -> verified GeometryIR
    ShadingGraph  -> verified ShadingIR
```

Core WGSL has no material-provided function pointers. Compilation therefore
selects one of several execution modes instead of interpreting every graph in
every traversal. Existing profiles remain specialized built-in programs and
provide a no-regression compatibility path. Small programs may use a bounded
instruction stream, while complex heightfield or volume programs materialize a
compact derived mask when a terrain page becomes resident.

### Material handles are stable runtime slots

Brick palettes reference a stable `VoxelMaterialHandle`, not a descriptor
deduplicated solely by its current bytes. Replacing a material program updates
the referenced slot while resident pages keep the same handle.

Immutable imported materials may still share slots as an optimization. An
editable terrain or editor material receives a unique stable slot.

### Determinism is part of the ABI

Geometry may use only:

- canonical signed integer parent and microcell coordinates;
- a material seed;
- quantized context;
- integer or defined fixed-point operations;
- deterministic bounded noise and hashes;
- material constants.

Geometry may not use frame number, temporal jitter, pixel coordinate, camera
position, derivatives, hardware texture filtering, uncontrolled floating-point
world hashing, or unbounded loops.

Shading randomness uses the same stable identity but may use floating-point
colour operations because it does not affect occupancy.

## Runtime flow

```text
terrain/package sample
    -> base voxel occupancy and material assignment
    -> packed MicroVoxelContext
    -> brick material handle + context sidecar

brick visibility
    -> parent occupied candidate
    -> load VoxelMaterialHandle
    -> load geometry program and context
    -> evaluate heightfield or volume occupancy
    -> exact hit / continue parent traversal / stable fallback
    -> write exact depth, face, identity, material handle, micro payload

material G-buffer
    -> load material and shading program
    -> read exact hit attributes and context
    -> evaluate colour and PBR properties
    -> write normal, base colour, roughness, metallic, and emissive
```

### Visibility-to-shading context

The material G-buffer cannot recover the source terrain page from the current
resolved surface, and the existing 32-bit micro payload is already occupied by
the material handle, microcell coordinates, and representation flags. Do not
make the material pass search brick data again.

The visibility draw first emits the normalized eight-bit terrain context in
the unused high byte of the existing packed bottom-albedo word:

```text
resolved bottom/context = bottom RGB24 | normalized context << 24
```

The material pass only decodes this byte for packed brick materials. This
already carries the surface class needed to distinguish flat terrain, slopes,
and walls. Runtime slope surfaces use the high bit as the surface flag and the
remaining seven bits as a normalized 0..90-degree slope angle, with no new
render target, resolve copy, or material-pass read.

Materials that require hit-dependent values not representable by that byte use
an opt-in compact resolved shading context beside the existing micro payload:

```cpp
struct ResolvedVoxelShadingContextGPU
{
  uint8_t microSlope;
  uint8_t terrainSlope;
  uint8_t depthBelowSurface;
  uint8_t terrainBlend;
};
```

The brick resolve pass copies this value into a screen-sized `u32` buffer, just
as it currently copies the micro payload. Promote the render attachment to
`RG32Uint` when supported, with payload in X and shading context in Y;
otherwise retain two `R32Uint` attachments. The buffer is created only when
the visible material set requests depth, terrain blend, or another auxiliary
scalar.

This keeps the material pass independent of terrain-page addressing. The
common slope/context path costs zero additional bytes; the optional
extended path costs four bytes per internal-resolution pixel and must be
enabled only after measuring attachment, resolve, and material-read bandwidth.

## Source material model

Add a renderer-independent asset description under a voxel material module:

```cpp
using VoxelMaterialHandle = uint16_t;

enum class MicroGeometryDomain : uint8_t
{
  Solid,
  TerrainHeightfield,
  FaceHeightfield,
  Volume,
};

struct VoxelMaterialAsset
{
  std::string name;
  uint32_t seed = 0;

  MicroGeometryGraph geometry;
  MicroShadingGraph shading;

  VoxelMaterialContextRequirements contextRequirements;
  SparseVoxelMaterialGPU defaults;
};
```

`SparseVoxelMaterialGPU` remains the per-voxel/palette surface record. Its
primary and secondary colours may vary within one material family. The new
material slot stores programs, constants, context requirements, and revisions
shared by those palette records.

## GPU material model

Replace the long-term role of the current eight-byte combined descriptor with a
stable table record:

```cpp
struct SparseVoxelMaterialProgramGPU
{
  uint32_t geometryHeader;
  uint32_t geometryProgramOffset;
  uint32_t geometryParameterOffset;
  uint32_t shadingHeader;
  uint32_t shadingProgramOffset;
  uint32_t shadingParameterOffset;
  uint32_t contextRequirements;
  uint32_t revisions;
};
```

This is 32 bytes per material slot, not per voxel. A full set of 4096 slots is
128 KiB. Program instructions and parameters live in compact global buffers.

`revisions` contains independent geometry and shading revisions. Revision zero
is reserved for the disabled/legacy material.

During migration, the existing `SparseVoxelMicroMaterialGPU` becomes the
version-one built-in-program encoding. The runtime material manager translates
it into a `SparseVoxelMaterialProgramGPU` slot so the renderer can support old
fixtures while new assets use the program representation.

## Context model

### Required built-ins

Every evaluation receives these values without optional storage:

```cpp
struct MicroVoxelBuiltins
{
  int3 canonicalParentKey;
  uint parentScale;
  uint3 microCell;
  uint subdivision;
  uint face;
  uint materialSeed;
};
```

### Base surface context

Normalize the current terrain context into one explicit 32-bit record:

```text
bits  0..23  four UNorm6 retained corner heights
bits 24..31  surface class and flags
```

Surface classes initially include:

- interior;
- flat/top surface;
- terrain transition surface;
- exposed -X, +X, -Z, or +Z wall;
- reserved object-authored surface.

This removes the current dependency on descriptor-handle upper bits for terrain
context and makes context vary correctly per occupied parent rather than per
palette entry.

### Optional contextual fields

Add a second logical context record for advanced materials:

```cpp
struct MicroVoxelAuxContextGPU
{
  uint8_t slope;
  uint8_t depthBelowSurface;
  uint8_t terrainBlend;
  uint8_t auxiliary;
};
```

`auxiliary` is interpreted through the material's declared context layout and
may initially represent moisture, snow amount, erosion, or mountain weight.

Do not allocate four additional bytes for every occupied voxel by default.
Store optional auxiliary records in a page-local exact pool and carry a packed
16-bit index only for pages whose active materials request auxiliary context.
Index zero supplies canonical defaults. Measure this representation against a
direct parallel buffer before committing the serialized format.

The material compiler emits `VoxelMaterialContextRequirements`. A page must be
rebuilt only when a new material version requests context fields that the page
did not bake.

## Terrain surface and slope evaluation

### Base height

For a terrain-heightfield material, evaluate the existing four-corner patch at
the microcolumn centre:

```text
u = (microX + 0.5) / subdivision
v = (microZ + 0.5) / subdivision
baseHeight = bilerp(h00, h10, h01, h11, u, v)
```

Quantize only the final retained height. Do not independently quantize patch
corners per material.

### Slope value

The default slope input is derived from the same patch, so geometry and shading
cannot disagree with the visible surface. Terrain may additionally bake a
wider-filtered slope into auxiliary context for biome-scale decisions.

Expose both meanings explicitly:

- `microSlope`: derivative of the local surface patch;
- `terrainSlope`: optional wider-filtered terrain derivative.

The local value controls geometric alignment and exposed risers. The wider
value controls behavior such as grass coverage or snow retention without
making individual microsteps feed back into material selection.

### Material modifier

A terrain material returns a signed fixed-point height modifier:

```text
finalHeight = clamp(baseHeight + modifier, 0, 1)
occupied = microcellTop <= quantize(finalHeight)
```

The modifier must sample canonical world-space noise. Adjacent parents must
therefore evaluate the same field at a shared physical boundary. The compiler
rejects parent-local random inputs for programs marked boundary-continuous.

Flat materials set the modifier to zero. Bumps use a small positive and
negative modifier. Carvings subtract height. Snow or soil deposition adds
height only where an already allocated parent envelope exists.

### Walls and steep transitions

Terrain top surfaces use the heightfield evaluator. Exposed wall parents remain
solid unless the material enables face carving or a volume program. The exact
hit face still determines whether the shading program displays the surface
layer or the underlying material.

This preserves closed terrain columns and prevents a steep slope from opening
cracks between its top and wall representation.

## Geometry IR

### Initial instruction set

Support a deliberately small typed instruction set:

- fixed-point constant and material parameter;
- canonical coordinate and face coordinate;
- base surface height;
- micro slope, terrain slope, depth, terrain blend, and auxiliary context;
- add, subtract, multiply, minimum, maximum, absolute, clamp, and saturate;
- deterministic hash noise in 2D and 3D;
- deterministic lattice noise at approved integer frequencies;
- step, range, smooth-range approximation, and select;
- quantize-to-microcell;
- heightfield occupancy;
- volume union, intersection, and subtraction.

The program verifier enforces:

- a maximum instruction count, initially 24;
- no backward jumps or loops;
- a maximum stack/register count;
- valid context reads declared by the material;
- subdivisions from two through eight;
- a statically known geometry domain;
- a conservative maximum inward modification;
- no unsupported opcode reaching GPU upload.

### Geometry execution policy

Geometry evaluation sits inside parent and microcell traversal, so a general
interpreter is not the default production path. After optimization, the
compiler selects:

```cpp
enum class GeometryExecutionMode : uint8_t
{
  BuiltIn,
  DirectIR,
  CachedHeightfield,
  CachedVolume,
};
```

- `BuiltIn` executes a specialized WGSL branch with material parameters. This
  is the expected path for grass, soil, snow, basic rock, and the migrated
  terrain surface program.
- `DirectIR` is restricted to a smaller traversal-time budget, initially 8 to
  12 simple instructions after constant folding.
- `CachedHeightfield` evaluates the program asynchronously per relevant parent
  and stores `N x N` quantized retained depths.
- `CachedVolume` evaluates the program asynchronously and stores an `N^3` bit
  mask.

Cached data is disposable and keyed by page ID, parent key, material handle,
geometry revision, context revision, and subdivision. A geometry edit with the
same context requirements regenerates only this derived data; it does not
rebuild authoritative terrain occupancy or material assignment. The old cache
remains visible until its replacement is atomically published.

### Built-in compatibility programs

Represent current profiles as built-ins:

- solid/disabled;
- constant face carve;
- hash columns;
- clumped columns;
- volume density;
- terrain base surface;
- terrain base surface plus hash bumps.

The current staircase descriptor maps to `terrain base surface` plus its
existing wall rules. Once reference images and tests match, remove the special
staircase branches from the generic evaluator.

### Coverage fallback

Programmable geometry must retain a stable sub-pixel fallback. Built-ins keep
their existing analytic coverage. General programs use one of:

1. a compiler-generated analytic estimator for supported height graphs; or
2. a fixed set of canonical stratified samples evaluated with the same program.

The estimator may not use frame-random sampling. Its selector is keyed by the
stable parent identity and material geometry revision. Expensive estimators may
be cached with disposable terrain-page data after profiling.

## Shading IR

### Inputs

The shading program receives:

- primary and secondary palette colours;
- primary and secondary material properties;
- exact or fallback micro payload;
- normalized micro depth;
- resolved geometric face and normal;
- whether the hit is a top, riser, wall, or volume boundary;
- stable parent and microcell random values;
- micro slope, terrain slope, depth, terrain blend, and auxiliary context;
- geometric LOD handoff weight.

### Initial instruction set

The first implemented subset provides eight scalar registers and these bounded
operations:

- normalized micro depth, stable random, normalized terrain-slope angle,
  side/riser classification, and constant inputs;
- add, multiply, maximum, scale-bias, one-minus, clamp, and smoothstep;
- a required primary/secondary layer-weight output;
- optional bounded hue/chroma strength, brightness-multiplier, roughness, and
  metallic outputs.

Programs are limited to 16 instructions. Verification rejects unknown opcodes,
out-of-range or uninitialized registers, non-finite constants, invalid
smoothstep edges, duplicate outputs, and calculations after the output phase.
Each stable material handle owns a fixed 16-instruction GPU range, allowing live
replacement without changing buffer size or offsets. Empty programs take the
existing profile switch before the interpreter loop.

Colour outputs are applied only after the primary/secondary ramp. Hue/chroma
uses a separate stable hash vector and approximately preserves luminance;
brightness is a non-negative multiplier. Neither colour output can affect visibility,
depth, normals, material identity, or temporal identity. The sample exposes
both values as live controls and enables mild authored variation on beach,
grass, and grassland terrain by default.

Roughness and metallic outputs use a negative internal sentinel when absent,
so an old or colour-only program retains the authored material values exactly.
When present, both are clamped to `[0, 1]`, then flow through the existing
global material override and normal-variance roughness filters in the same
order as ordinary authored PBR values. One-colour materials may emit these
scalars even though their colour ramp degenerates to primary-to-primary.

Expand the instruction set with:

- scalar/vector constants and material parameters;
- add, multiply, min, max, clamp, remap, smoothstep, and select;
- linear RGB colour ramp;
- optional perceptual colour ramp after measuring its cost;
- hue/chroma variation with approximately preserved luminance;
- brightness and contrast variation;
- stable hash noise;
- top/side/riser masks;
- roughness, metallic, and emissive ramps;
- bounded shading-normal perturbation.

The output is the existing material G-buffer contract. Shading instructions
cannot write depth, occupancy, hit face, or temporal geometry identity.

### Grass-over-dirt reference material

The first authored material is equivalent to:

```text
geometry:
    base = SampleTerrainSurface()
    flatness = 1 - SmoothRange(grassSlopeStart, grassSlopeEnd, terrainSlope)
    bump = HashNoise2D(worldMicroXZ, seed) * bumpStrength * flatness
    carve = SparseNoise2D(worldMicroXZ, seed + 1) * carveStrength
    OccupancyBelow(Quantize(base + bump - carve))

shading:
    sideSoil = IsRiserOrWall()
    depthSoil = Remap(grassDepthStart, grassDepthEnd, microDepth)
    slopeSoil = SmoothRange(dirtSlopeStart, dirtSlopeEnd, terrainSlope)
    soilWeight = Max(sideSoil, depthSoil, slopeSoil)
    colour = LinearRamp(grassGreen, soilBrown, soilWeight)
    colour = HueVariation(colour, StableNoise(), hueStrength)
    colour = BrightnessVariation(colour, StableNoise2(), brightnessStrength)
```

Diagnostic defaults use bright green and brown so geometry and layer selection
are easy to validate before art tuning.

The shading half of this reference is now implemented for the grass and
grassland sample materials. Their soil weight is the maximum of resolved
side/riser classification, a programmable smooth micro-depth transition, and
the normalized terrain-slope angle. The sample exposes the two slope thresholds
in degrees and compiles them into a bounded `Smoothstep` instruction. The slope
uses the generator's deterministic 3x3 Scharr derivative and occupies the seven
free bits of the already-resolved context byte, so it adds no attachment or
material-pass lookup. The optional extended context word remains reserved for
independent fields such as terrain blend. Geometry remains the existing
deterministic staircase program until Geometry IR is introduced.

The reference grass program also reuses `soilWeight` to blend programmable
grass and soil roughness. This adds one scale-bias instruction and one output,
keeping the default hue, brightness, colour, and roughness program at 15 of the
16 available instructions. Metallic remains inherited at zero for grass, while
the general IR supports explicit metallic outputs for rock or ore materials.

## Change classification and invalidation

| Change | Page rebuild | Renderer invalidation |
| --- | --- | --- |
| Primary/secondary colour or PBR parameter | No | Material and lighting history |
| Shading parameters | No | Material and lighting history |
| Shading program, same context requirements | No | Material and lighting history |
| Geometry parameters, same context requirements | No | Visibility, depth, HZB, motion, and temporal history |
| Geometry program, same context requirements | No | Visibility, depth, HZB, motion, and temporal history |
| Material requests a context channel absent from a page | Yes | Rebuild affected pages and visibility history |
| Terrain material assignment or source terrain changes | Yes | Rebuild affected pages and visibility history |

Geometry revision participates in exact micro surface identity. Shading revision
does not make the surface appear to move.

## Serialization and assets

Add a versioned voxel material asset containing:

- material name and stable asset ID;
- default primary/secondary colours and PBR values;
- seed;
- geometry graph or compiled IR plus parameters;
- shading graph or compiled IR plus parameters;
- context requirements;
- compiler version and content hash.

Runtime scene and terrain packages reference stable asset IDs. The runtime
material manager assigns compact handles when assets are loaded.

Transient terrain pages serialize context data and handles, not copies of
material programs. Old terrain/page versions load through the built-in profile
adapter. Unknown opcodes, missing assets, or invalid programs fall back
conservatively to a solid parent and the primary palette colour.

## Implementation phases

### Phase 0 — lock references and measurements

Status: complete. Deterministic signatures, memory accounting, CPU/WGSL
coverage agreement, capture commands, and the current disabled-path GPU timing
reference are recorded in `docs/voxel-material-phase0-baselines.md`.

- Record current grass, slope staircase, hash-column, clumped-column, and
  volume-density output signatures.
- Capture brick visibility and material G-buffer timings for disabled, slope,
  heightfield, and volume cases.
- Extend CPU/GPU agreement tests to cover the existing slope coverage patch at
  subdivisions two through eight.
- Record terrain hierarchy and transient-page memory attributable to
  `microSurfaceData` and palette descriptor IDs.

Exit criteria:

- deterministic baselines are reproducible;
- current disabled and exact paths have reference outputs and timings;
- no new representation is accepted without comparison to these baselines.

### Phase 1 — first-class stable material slots

Status: complete. First-class assets, stable editable/immutable slots, legacy
adapters, authored terrain defaults, independent revisions, and live updates
are covered by `VoxelMaterialTests`.

- Add `VoxelMaterialAsset`, `VoxelMaterialHandle`, and
  `VoxelMaterialContextRequirements`.
- Add a runtime `VoxelMaterialManager` owning stable GPU slots, program buffers,
  parameters, revisions, and asset-to-handle mapping.
- Adapt `SparseVoxelMicroMaterialLibrary` and existing descriptors into built-in
  material slots.
- Preserve handle zero as disabled/legacy.
- Move default biome presets out of the sample into authored/default voxel
  material definitions.
- Keep visuals unchanged.

Exit criteria:

- resident terrain keeps valid handles after a live material update;
- old tests and output signatures remain unchanged;
- sample code no longer contains biome-specific geometry/shading switches;
- immutable material deduplication cannot merge independently editable slots.

### Phase 2 — normalize base surface and context

Status: complete for the raster/material path. Packed base context, quantitative
slope, page/clipmap propagation, resolved spare-byte transport, optional packed
extended-context ABI, transient persistence/version diagnostics, and seam tests
are in place. Producers can opt into the extended context word when terrain
blend or auxiliary hit scalars are needed.

- Introduce the explicit packed base-surface context record.
- Move surface class out of palette/descriptor bits and into per-parent context.
- Teach brick build, clipmap combination, page upload, spill/restore, and
  revision hashing to carry the normalized context.
- Factor terrain patch sampling and wall classification into shared CPU/WGSL
  functions.
- Propagate the normalized surface classification through brick draw, resolve,
  and material G-buffer using the spare bottom-albedo byte.
- Add an opt-in resolved shading-context attachment/buffer for depth, terrain
  blend, and any later hit-dependent scalar that does not fit the common byte.
- Map the existing staircase profile to the terrain base-surface evaluator.

Exit criteria:

- flat and sloped terrain match current reference depth and silhouettes;
- material palette cardinality no longer increases because two parents have
  different surface classes;
- adjacent pages and clipmap levels share slope boundaries without cracks;
- the material pass reads context without resolving a source brick/page;
- old serialized pages either migrate or fail with a clear version error.

### Phase 3 — programmable shading

Status: complete. The verified bounded IR has CPU/WGSL evaluators, fixed stable
instruction ranges, per-slot parameter blocks, all five compatibility profiles,
grass/soil masks, colour/PBR outputs, safe invalid-program fallback, and live
program/parameter uploads.

- Define `ShadingIR`, its verifier, CPU reference evaluator, and WGSL evaluator.
- Add instruction and parameter buffers to the material G-buffer pass.
- Implement two-colour ramp, depth/slope masks, stable hue and brightness
  variation, exposed-side selection, and PBR ramps.
- Convert current Linear, HardCrust, NoisyLinear, ExposedSides, and FoliageTint
  profiles into built-in shading programs.
- Add live shading compilation and slot upload.

Exit criteria:

- changing a shading program never changes depth or micro identity;
- grass-over-dirt works on flat surfaces, slopes, and risers;
- hue and brightness variation is stable under static jitter and camera motion;
- resident pages are not rebuilt for shading changes;
- invalid programs fall back to primary colour.

### Phase 4 — programmable terrain geometry modifiers

Status: complete for the brick/material path. Fixed Q16.16 Geometry IR,
verification, CPU and WGSL evaluators, stable per-slot GPU ranges, execution
selection, parent heightfield/volume materializers, world-stable noise,
terrain-patch composition, live revision invalidation, and deterministic
coverage fallback are implemented and covered by `VoxelMaterialTests`.

- Define fixed-point `GeometryIR`, verifier, CPU evaluator, and WGSL evaluator.
- Add compiler execution selection for built-in, small direct, cached
  heightfield, and cached volume programs.
- Implement terrain base surface, world-stable noise, arithmetic, context masks,
  quantization, and heightfield occupancy.
- Compose material modifiers with the base terrain patch.
- Convert constant, hash-column, clumped-column, and staircase behavior into
  built-in or equivalent instruction programs.
- Carry geometry revision into temporal identity and invalidation.
- Implement analytic or stratified stable coverage fallback.

Exit criteria:

- one material can switch among flat, lightly bumped, strongly bumped, and
  carved terrain without rebuilding pages;
- CPU and GPU occupancy agree for every microcell in the reference fixtures;
- shared page boundaries contain no noise seams;
- complex programs materialize once per affected parent rather than being
  interpreted for every traversing fragment;
- geometry edits invalidate visibility history but retain material handles;
- exact-to-fallback transitions contain no distance ring or shimmer.

### Phase 5 — volume programs and contextual fields

Status: complete for the brick/material path. Geometry IR exposes normalized
micro slope, terrain slope, depth, terrain blend, and auxiliary inputs and
implements bounded volume union, intersection, and subtraction. Legacy volume
density now has a verified Volume IR representation. Optional context uses a
deduplicated page-local `u32` pool with `u16` per-voxel indices and a canonical
zero default; pages with no optional requirements allocate nothing. The Phase
5 fixture uses 264 bytes for 128 voxels and two unique contexts versus 512 bytes
for a direct `u32` sidecar. Compiler diagnostics name every unavailable field,
and page rebuild selection filters by material handle and missing newly-required
fields. Snow retention, porous rock, foliage envelope, depth, traversal, and
memory cases are covered by `VoxelMaterialTests` under `svraster.phase5`.

- Add the volume-domain occupancy result and approved CSG operations.
- Convert volume-density behavior to Geometry IR.
- Add optional slope, depth, terrain blend, and auxiliary context production.
- Implement and measure the optional context pool/index representation.
- Add material compiler diagnostics for unavailable context.
- Add snow retention, porous rock, and foliage-envelope fixtures.

Exit criteria:

- volume traversal agrees with brute-force microcell intersection;
- depth-sensitive materials distinguish topsoil from deep material;
- steep slopes can reduce grass or snow without changing the base terrain;
- context memory stays within the budget established in Phase 0;
- requesting a newly required context field rebuilds only affected pages.

### Phase 6 — assets, editor, and persistence

Status: complete. Voxel material assets use a checksummed versioned binary
format and content-addressed compiler cache. Runtime loads and hot reloads keep
stable material handles, terrain transient pages and optional scene chunks
preserve stable asset identity/context, and legacy scene files remain readable.
The sample selects verified graph presets with parameter controls and exposes
material handle/program/context/base/modifier/occupancy/layer/exact-fallback
debug outputs. CPU round-trip, cache, hot-reload, diagnostic, transient-page,
scene compatibility, WGSL, and interactive-sample build coverage is in place.

- Add the versioned voxel material file format and compiler cache.
- Add material loading and hot reload to the runtime manager.
- Replace profile-specific sample controls with graph/asset selection and
  parameter controls.
- Serialize material asset references and context data in terrain and scene
  paths.
- Add debug views for material handle, program ID, context, base height,
  modifier, final occupancy, layer weight, and exact/fallback state.

Exit criteria:

- a material asset round-trips without changing compiled output;
- scene reload and transient terrain spill/restore preserve material identity;
- compiler errors identify the offending node and constraint;
- old scenes render through the compatibility adapter.

### Phase 7 — ray-path parity and optimization

Status: complete. Exact Tree64 leaves optionally resolve a bounded sparse
material/context sidecar and run the same fixed-point Geometry IR and exact
micro-DDA as brick visibility. Static, dynamic, closest-hit, any-hit, primary,
shadow/GI, region-marcher, and mirrored-ancestor shader variants share this
leaf acceptance code. A fully carved parent advances to its half-open exit and
continues traversal. Disabled scenes select the original SPIR-V and original
binding layout, so the Phase 0 path has no added instruction, context load, or
bandwidth. GPU counters expose candidate parents, sidecar probes, context
loads, interpreted instructions, microcell steps, carved parents, and accepted
parents; the CPU reference also records instruction-stream divergence and
incremental material-pass bandwidth. Measurement justified separate compiled
variants, but did not justify material binning, cached masks, or node summaries.
The sparse sidecar is exact-only: coarse Tree64 cells that combine multiple
source materials omit their record and remain conservatively occupied instead
of receiving an arbitrary material. A future measured optimization may replace
that fallback with cached masks or node summaries. The sample automatically
activates material-aware static Tree64 tracing for grid-aligned finest brick
parents. It records why other parents were omitted and leaves transformed
coarse dynamic terrain on the original conservative trace variant.

- Share Geometry IR and context decoding with Tree64 primary, shadow, and probe
  leaf-hit traversal.
- Continue Tree64 traversal after a completely carved parent.
- Measure interpreter cost, instruction divergence, context loads, and material
  pass bandwidth.
- Add compiled shader variants, material binning, cached masks, or node summaries
  only when end-to-end measurements justify them.

Exit criteria:

- raster, Tree64, shadow, and probe rays agree on reference hits;
- disabled materials remain within one percent of the Phase 0 baseline;
- simple heightfield materials remain within the existing microgeometry budget;
- no optimization changes deterministic occupancy or identity.

### Phase 8 — bounded material fields and planet-region mappings

Status: complete. Assets declare four bounded scalar field slots with
semantics, source, mapping, sampling policy, normalized fallback, scale, bias,
and an optional logical texture identity. Geometry and shading IR expose
verified `LoadField` operations with CPU and WGSL evaluators. One 16-byte
Q16.16 default record is uploaded per stable material handle and shared by
brick geometry, Tree64 geometry, and material shading. Assets without field
declarations keep their authored fallback behavior. Resolved micro surfaces
carry canonical signed micro-grid coordinates and a deterministic logical
planet-region ID in a 16-byte companion record; physical page, brick, clipmap,
and atlas slots are not exposed. Material texture identities and logical
planet regions now resolve through an eight-probe bounded table into one
shared RGBA8Uint atlas. Each channel is one logical scalar field, sampling
happens before Shading IR evaluation, and absent mappings retain authored
constants. Planet clipmap generation produces deterministic grass, dirt, and
colour-variation tiles in planet space. Endpoint sampling makes neighbouring
region borders exact, coincident parent/child samples agree across LODs, and
streaming commits rebuild the bounded mappings without changing GPU bindings.
The default Terrain Shading IR uses generated hue variation plus deterministic
microvoxel brightness variation on flat tops; its existing slope, side/riser,
and depth masks remain the only default soil selectors. The coarse exact-micro
fallback treats a terrain staircase as a visible zero-depth surface, so the
same grass layer survives the near-to-horizon handoff while quantitative slope
continues to select soil. Generated grass and dirt coverage stay bound for
custom graphs without imprinting their sampling pattern on the default terrain.
Geometry programs that read a field are compiled to cached heightfield or
cached volume execution. Cache materialization uses nearest mip-zero integer
samples in Q16.16, is shared by
brick raster and Tree64 consumers, and treats absent tiles or cache entries as
occupied. Geometry/material/atlas content revisions rebuild the cache and
invalidate visibility history before the new occupancy is used. Shading now
supports a four-colour bounded palette: entries zero and one remain the
existing per-voxel primary/secondary pair, while entries two and three are
material-owned RGBA8 values. Two optional IR outputs produce normalized layer
weights; programs that do not use them retain the exact two-colour fast path.
The initial terrain materials continue using only grass and dirt.

1. **Complete.** Define four fixed material-field descriptors and defaults. Add a statically
   indexed `LoadField` operation to Geometry IR and Shading IR, including CPU
   reference evaluation, WGSL evaluation, required-field masks, verifier
   limits, asset persistence, and stable-slot revision handling.
2. **Complete.** Back every declared field with its authored constant fallback. Upload one
   compact default-value record per stable material slot, without allocating
   textures or changing existing terrain visuals.
3. **Complete.** Add a stable material sample address beside resolved surfaces. Carry a
   logical region mapping ID and the minimum canonical micro address required
   by shading; never expose transient physical brick or page slots.
4. **Complete.** Add one shared R8/RGBA8 field atlas or texture array and a bounded region
   mapping table. Resolve material-owned texture identities and planet-region
   overrides into the same four logical field slots.
5. **Complete.** Generate grass, dirt, and colour-variation tiles during planet-region
   generation with deterministic seeds, shared borders, and parent/child LOD
   continuity. Combine these values with the existing terrain-slope input in
   Shading IR.
6. **Complete.** Permit deterministic geometry fields only through fixed-mip integer loads
   and cached heightfield/volume materialization. Missing geometry tiles use a
   conservative no-carve fallback and geometry content revisions invalidate
   visibility history.
7. **Complete.** Extend the two-colour surface ramp to a bounded palette when simultaneous
   snow, grass, dirt, and rock layers are required. Keep the initial terrain
   integration on the existing grass/dirt pair.

Implemented budgets:

- four declared field slots per material;
- at most two distinct fields in one shading program;
- at most one distinct field in one geometry program;
- four palette colours, with only two auxiliary colours stored per material;
- normalized palette weights with no unbounded layer accumulation;
- statically encoded field indices only;
- missing or non-resident shading sources return their authored normalized
  default; geometry sources use conservative occupied fallback;
- texture access remains outside the interpreters and uses explicit mapping,
  filtering, addressing, and LOD policies.

Exit criteria:

- material programs can replace stable random colour selection with bounded
  field values without changing material handles;
- current slope and subvoxel colour behavior is unchanged when fields are not
  bound;
- planet-region tiles do not seam across region or clipmap boundaries;
- raster and Tree64 geometry consumers agree for every field-driven carve;
- non-resident geometry data cannot produce false empty space;
- field sampling and cache lookup obey the bounded-cost contract in
  `docs/voxel-material-phase8-validation.md`; GPU comparisons use the fixed
  Phase 0 capture protocol.

## File-level work map

### New core material module

- `src/voxel/material/VoxelMaterialAsset.hpp`
- `src/voxel/material/VoxelMaterial.hpp`
- `src/voxel/material/VoxelGeometryIR.hpp`
- `src/voxel/material/VoxelShadingIR.hpp`
- `src/voxel/material/VoxelMaterialContext.hpp`
- `src/voxel/material/VoxelMaterialField.hpp`
- `src/voxel/material/VoxelMaterialFieldAtlas.hpp`
- `src/voxel/material/VoxelBuiltInShading.hpp`
- `src/voxel/material/VoxelTerrainMaterialPresets.hpp`
- corresponding `.cpp` implementations
- corresponding CMake registration and tests

### Existing CPU and ABI integration

- `src/voxel/svraster/SparseVoxelGPU.hpp`
- `src/voxel/svraster/SparseVoxelMicroMaterial.hpp`
- `src/voxel/svraster/SparseVoxelMicroMaterial.cpp`
- `src/voxel/svraster/SparseVoxelMicroGeometry.hpp`
- `src/voxel/svraster/SparseVoxelMicroGeometry.cpp`
- `src/voxel/svraster/SparseVoxelGeometryFieldCache.hpp`
- `src/voxel/svraster/SparseVoxelGeometryFieldCache.cpp`
- `src/voxel/svraster/SparseVoxelBrick.hpp`
- `src/voxel/svraster/SparseVoxelBrick.cpp`
- `src/voxel/svraster/PlanetVoxelChunk.cpp`
- transient page and scene-file serialization paths

### Shader and pass integration

- `assets/shaders/voxel/svraster/wgsl/sparse_voxel_micro_geometry.wgsl`
- `assets/shaders/voxel/svraster/wgsl/voxel_geometry_ir.wgsl`
- `assets/shaders/voxel/svraster/wgsl/voxel_geometry_field_cache.wgsl`
- `assets/shaders/voxel/svraster/wgsl/voxel_material_field_atlas.wgsl`
- `assets/shaders/voxel/svraster/wgsl/sparse_voxel_brick_draw.wgsl`
- `assets/shaders/voxel/svraster/wgsl/sparse_voxel_brick_resolve.wgsl`
- `assets/shaders/voxel/svraster/wgsl/sparse_voxel_material_gbuffer.wgsl`
- `src/voxel/svraster/rendering/SparseVoxelBrickPasses.hpp`
- `src/voxel/svraster/rendering/SparseVoxelGBufferPass.hpp`
- `src/voxel/svraster/rendering/SparseVoxelTree64TracePass.hpp`
- `src/voxel/svraster/rendering/SparseVoxelTree64DynamicTracePass.hpp`

### Authoring and validation

- `samples/sparseVoxelRenderer/SparseVoxelRendererSample.cpp`
- `tests/voxel/SparseVoxelMicroGeometryTests.cpp`
- `tests/voxel/SparseVoxelPlanetChunkTests.cpp`
- scene-file, streaming, raster-depth, temporal-stability, and GPU agreement
  tests

## Validation matrix

| Area | Required coverage |
| --- | --- |
| Base surface | flat patches, all slope directions, shared corners, page seams, clipmap seams |
| Geometry programs | verifier failures, all opcodes, N=2/5/8, fixed-point CPU/GPU agreement |
| Context | missing/default fields, slope, depth, terrain blend, page serialization |
| Shading | colour endpoints, hue/brightness bounds, top/riser/wall behavior, PBR outputs |
| Determinism | worker counts, static jitter, camera motion, page replacement, reload |
| Live editing | shading-only, geometry-only, context-requiring, invalid program fallback |
| LOD | exact/fallback transition, MetalFX, horizon, clipmap ownership |
| Traversal | complete holes, later parent, later brick, heightfield and volume rays |
| Consumers | depth, HZB, GTAO, motion, screen traces, temporal identity, shadows, probes |
| Performance | disabled, flat slope, modified heightfield, volume, context on/off |

## Reference visual scenarios

1. Flat green grass over dark brown soil with no modifier.
2. The same material with one-microcell world-continuous bumps.
3. A continuous hillside where top microfaces remain green and risers expose
   soil.
4. A steep hillside where terrain slope progressively removes grass coverage.
5. Snow accumulation on flat terrain and thinning on slopes.
6. Clumped rock carving across a terrain-page boundary.
7. A filled foliage envelope carved by a volume program.
8. Live colour, shading, and geometry edits while terrain pages remain resident.

Each scenario needs top, grazing, silhouette, native-resolution, upscaled, and
static-history captures.

## Risks and controls

- **Geometry interpreter cost:** retain built-ins, enforce a smaller direct-IR
  budget, and materialize complex programs into disposable height/mask caches.
- **CPU/GPU disagreement:** use fixed-point geometry, shared constants, and
  exhaustive microcell/ray agreement tests.
- **Terrain cracks:** terrain owns shared corner heights; modifiers use canonical
  world coordinates and explicit boundary-continuity validation.
- **Context memory growth:** keep base context compact, allocate auxiliary
  context only when requested, and gate the representation with Phase 0
  measurements.
- **LOD shimmer:** reuse the existing projected-size handoff and require a stable
  coverage estimator for every geometry program.
- **History ghosts:** separate geometry and shading revisions and invalidate the
  correct consumers.
- **Off-screen lighting mismatch:** remain conservatively solid until Tree64
  parity lands, then share the same verified program rather than copying profile
  logic.
- **Unbounded material complexity:** reject loops, unsupported context reads,
  excessive instructions, and geometry outside the parent envelope at compile
  time.

## Non-goals for the first implementation

- arbitrary user-provided WGSL or native shader code;
- animated or time-dependent occupancy;
- geometry extending beyond its conservative parent envelope;
- texture-sampled geometry decisions;
- subdivisions greater than eight;
- replacing the existing terrain generator or biome assignment system;
- allocating a dense microvolume for every parent voxel.

## Completion definition

The system is complete when voxel materials are authored as assets with stable
runtime handles; terrain supplies a reusable base surface and context; materials
can deterministically flatten, bump, deposit, erode, or carve that surface;
grass and soil can be blended from exact face, depth, slope, and stable noise;
shading and same-context geometry edits update without rebuilding resident
pages; page and clipmap boundaries remain seamless; CPU, raster, and ray paths
agree on occupancy; and legacy materials retain their disabled fast path.
