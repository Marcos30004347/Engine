#include "sparse_voxel_probe_common.wgsl"

// Probe ray sharing, following Brixelizer GI's FfxBrixelizerGIReprojectScreenProbes.
//
// An awake probe can trace up to 64 rays in a frame. That is a whole
// hemisphere, but it is still only 64 samples, and settled or neighbour-seeded
// probes deliberately trace fewer.
//
// The observation this pass is built on is that a probe's neighbours already
// paid for their own 64 rays, and those rays are *reusable*. A stored cell is
// not just a radiance value: it is a radiance value together with the distance
// the ray travelled, which means the neighbour's hit point can be reconstructed
// in world space. Reconstructing that point and re-deriving the direction from
// *this* probe's position turns a neighbour's ray into a valid sample for this
// probe - parallax corrected, not merely blurred across probes. Nine probes of
// history therefore contribute up to 576 directional samples to a probe that
// only traced 64 itself.
//
// Blurring the neighbours' irradiance instead would be far cheaper and is what
// makes probe grids leak: it merges samples belonging to different points
// without ever accounting for the geometry between them. Reprojecting through
// the hit point keeps each sample attached to the surface that produced it, so
// a sample only lands in a direction it would genuinely have arrived from.
//
// The result is written as this frame's starting radiance. The resolve pass
// then blends each cell's freshly traced ray against it, so tracing and sharing
// compound instead of competing.

struct SparseVoxelProbeShareSettings {
    // x probes across, y probes down, z history valid, w frame index.
    probeCounts : vec4<u32>,
    // x normal weight power, y probe radius scale for the position falloff,
    // z minimum accepted neighbour weight, w unused.
    weights : vec4<f32>,
    // x probes in total, refinement overflow included, y the probe tile size in
    // pixels, z non-zero to let a settled probe skip the neighbourhood gather.
    // The neighbourhood search walks the uniform grid - that is where the
    // nine-probe neighbourhood lives - but the dispatch covers every probe,
    // refined ones included, and a refined probe is anchored by its tile.
    counts : vec4<u32>,
    // xyz camera world position, w analytical far-field cutoff in world units.
    cameraPositionCutoff : vec4<f32>,
}

@group(0) @binding(0) var<uniform> settings : SparseVoxelProbeShareSettings;
@group(0) @binding(1) var<storage, read> probes : array<SparseVoxelProbe>;
@group(0) @binding(2) var<storage, read> previousProbes : array<SparseVoxelProbe>;
@group(0) @binding(3) var<storage, read> previousRadiance : array<vec4<f32>>;
@group(0) @binding(4) var<storage, read_write> radiance : array<vec4<f32>>;
// The scheduling state the ray pass wrote last frame. This pass runs before
// this frame's ray pass, so the previous slot is the only one populated - which
// is also exactly what the ray pass itself reads, so the two agree on a probe's
// tier without needing to communicate.
@group(0) @binding(5) var<storage, read> previousSchedule :
    array<SparseVoxelProbeSchedule>;

// One bin per direction cell. Reprojection scatters: a neighbour's cell can
// land in any of this probe's cells, so the accumulation has to be atomic even
// though each lane owns one cell for reading.
var<workgroup> binRed : array<atomic<u32>, 64>;
var<workgroup> binGreen : array<atomic<u32>, 64>;
var<workgroup> binBlue : array<atomic<u32>, 64>;
var<workgroup> binWeight : array<atomic<u32>, 64>;
var<workgroup> binDistance : array<atomic<u32>, 64>;
// Probe-wide mean of everything that arrived, used to fill cells that no
// neighbour reached.
var<workgroup> meanRed : atomic<u32>;
var<workgroup> meanGreen : atomic<u32>;
var<workgroup> meanBlue : atomic<u32>;
var<workgroup> meanCount : atomic<u32>;

const PROBE_SHARE_RADIANCE_SCALE : f32 = 1024.0;
const PROBE_SHARE_WEIGHT_SCALE : f32 = 65536.0;
// 64 steps per world unit still resolves a hit to well under a voxel while
// keeping the sky sentinel inside the 32-bit range.
const PROBE_SHARE_DISTANCE_SCALE : f32 = 64.0;

// Round, never truncate.
//
// This pass is a feedback loop: it reads the previous frame's radiance map and
// writes this frame's, which it will read again next frame. A truncating
// quantiser loses up to one step on every value it carries, always downward, so
// the loop is a geometric decay rather than a fixed rounding error - the map
// dims a little every frame for as long as the camera holds still.
//
// The size of the loss is relative to the accumulated sum, so it stayed
// invisible while every probe gathered nine neighbours and traced all 64 of its
// directions every frame: the sum was large and fresh unbiased samples arrived
// constantly. It becomes plainly visible once a settled probe gathers a single
// source and re-measures a given direction only every few frames, which is
// exactly the combination the update scheduler produces. Rounding makes the
// error zero-mean, so it cancels over the loop instead of accumulating.
fn probe_share_quantise(value : f32, scale : f32) -> u32 {
    return u32(clamp(value * scale + 0.5, 0.0, 2.0e9));
}

// Scatter a reprojected direction to the four surrounding octahedral cells.
// Nearest-cell assignment makes tiny probe motion flip a sample between cells;
// because sharing is temporal feedback, that flip appears as a bright/dark
// sparkle rather than harmless one-frame quantisation. Bilinear splatting makes
// the same motion change the two cells' weights continuously.
fn probe_share_scatter(
    probeNormal : vec3<f32>, direction : vec3<f32>, value : vec3<f32>,
    travelled : f32, weight : f32) {
    let basis = sparse_voxel_probe_basis(probeNormal);
    let local = vec3<f32>(
        dot(direction, basis[0]), dot(direction, basis[1]),
        dot(direction, basis[2]));
    if (local.z <= 0.0) { return; }

    let uv = clamp(
        sparse_voxel_probe_hemioct_encode(local), vec2<f32>(0.0),
        vec2<f32>(0.999999));
    let texel = uv * f32(SPARSE_VOXEL_PROBE_OCTAHEDRAL_SIZE) -
        vec2<f32>(0.5);
    let base = vec2<i32>(floor(texel));
    let fraction = fract(texel);

    var accepted = 0.0;
    for (var y = 0i; y <= 1i; y += 1i) {
        for (var x = 0i; x <= 1i; x += 1i) {
            let coord = base + vec2<i32>(x, y);
            if (any(coord < vec2<i32>(0)) || any(coord >=
                vec2<i32>(i32(SPARSE_VOXEL_PROBE_OCTAHEDRAL_SIZE)))) {
                continue;
            }
            let wx = select(1.0 - fraction.x, fraction.x, x != 0i);
            let wy = select(1.0 - fraction.y, fraction.y, y != 0i);
            accepted += wx * wy;
        }
    }
    if (accepted <= 1.0e-6) { return; }

    for (var y = 0i; y <= 1i; y += 1i) {
        for (var x = 0i; x <= 1i; x += 1i) {
            let coord = base + vec2<i32>(x, y);
            if (any(coord < vec2<i32>(0)) || any(coord >=
                vec2<i32>(i32(SPARSE_VOXEL_PROBE_OCTAHEDRAL_SIZE)))) {
                continue;
            }
            let wx = select(1.0 - fraction.x, fraction.x, x != 0i);
            let wy = select(1.0 - fraction.y, fraction.y, y != 0i);
            let splatWeight = weight * wx * wy / accepted;
            if (splatWeight <= 1.0e-6) { continue; }
            let targetCell = u32(coord.y) *
                SPARSE_VOXEL_PROBE_OCTAHEDRAL_SIZE + u32(coord.x);
            atomicAdd(&binRed[targetCell], probe_share_quantise(
                value.x * splatWeight, PROBE_SHARE_RADIANCE_SCALE));
            atomicAdd(&binGreen[targetCell], probe_share_quantise(
                value.y * splatWeight, PROBE_SHARE_RADIANCE_SCALE));
            atomicAdd(&binBlue[targetCell], probe_share_quantise(
                value.z * splatWeight, PROBE_SHARE_RADIANCE_SCALE));
            atomicAdd(&binWeight[targetCell], probe_share_quantise(
                splatWeight, PROBE_SHARE_WEIGHT_SCALE));
            atomicMin(&binDistance[targetCell], probe_share_quantise(
                min(travelled, SPARSE_VOXEL_PROBE_SKY_DISTANCE),
                PROBE_SHARE_DISTANCE_SCALE));
        }
    }
}

// Reproject one previous-frame probe's cell into this probe's frame and scatter
// it into the bin it belongs to. Factored out because the neighbourhood is no
// longer only the 3x3 grid patch: a refined probe's own predecessor lives past
// the end of that grid and has to be gathered by index.
fn probe_share_gather(
    neighbourIndex : u32, cellIndex : u32, probePosition : vec3<f32>,
    probeNormal : vec3<f32>, epsilon : f32, normalPower : f32,
    minimumWeight : f32) {
    if (neighbourIndex >= arrayLength(&previousProbes)) { return; }
    let neighbour = previousProbes[neighbourIndex];
    if ((neighbour.identityState.z &
        SPARSE_VOXEL_PROBE_STATE_VALID) == 0u) { return; }

    let neighbourPosition = neighbour.positionDepth.xyz;
    let neighbourNormal = neighbour.normalRadius.xyz;
    let orientation = pow(
        clamp(dot(probeNormal, neighbourNormal), 0.0, 1.0), normalPower);
    let proximity = clamp(
        1.0 - length(neighbourPosition - probePosition) / epsilon, 0.0, 1.0);
    let weight = orientation * proximity;
    if (weight <= minimumWeight) { return; }

    let sourceIndex =
        neighbourIndex * SPARSE_VOXEL_PROBE_DIRECTION_COUNT + cellIndex;
    if (sourceIndex >= arrayLength(&previousRadiance)) { return; }
    let stored = previousRadiance[sourceIndex];
    if (stored.w < 0.0) { return; }

    let sourceDirection = sparse_voxel_probe_cell_direction(
        neighbourNormal, cellIndex);
    var direction = sourceDirection;
    var travelled = SPARSE_VOXEL_PROBE_SKY_DISTANCE;
    if (stored.w < SPARSE_VOXEL_PROBE_SKY_DISTANCE) {
        // Parallax correction: the sample belongs to a point, not to a
        // direction, so it has to be re-aimed from this probe's origin.
        let hitPoint = neighbourPosition + sourceDirection * stored.w;
        let offset = hitPoint - probePosition;
        let length2 = dot(offset, offset);
        if (length2 <= 1.0e-8) { return; }
        travelled = sqrt(length2);
        direction = offset / travelled;
    }

    // Nearest contributor wins each destination's distance. Averaging a two
    // metre hit with a sky miss would produce a hit point that exists nowhere
    // and would then be reprojected again next frame.
    probe_share_scatter(
        probeNormal, direction, stored.xyz, travelled, weight);
}

@compute @workgroup_size(64, 1, 1)
fn shareSparseVoxelProbeRays(
    @builtin(workgroup_id) workgroupId : vec3<u32>,
    @builtin(local_invocation_index) localIndex : u32) {
    let probeIndex = workgroupId.x;
    let probeCounts = settings.probeCounts.xy;
    let uniformCount = probeCounts.x * probeCounts.y;
    let probeCount = max(settings.counts.x, uniformCount);
    let cellIndex = localIndex;

    atomicStore(&binRed[cellIndex], 0u);
    atomicStore(&binGreen[cellIndex], 0u);
    atomicStore(&binBlue[cellIndex], 0u);
    atomicStore(&binWeight[cellIndex], 0u);
    atomicStore(&binDistance[cellIndex], 0xffffffffu);
    if (localIndex == 0u) {
        atomicStore(&meanRed, 0u);
        atomicStore(&meanGreen, 0u);
        atomicStore(&meanBlue, 0u);
        atomicStore(&meanCount, 0u);
    }
    workgroupBarrier();

    if (probeIndex >= probeCount || probeIndex >= arrayLength(&probes)) {
        return;
    }
    let cellBase = probeIndex * SPARSE_VOXEL_PROBE_DIRECTION_COUNT;
    if (cellBase + cellIndex >= arrayLength(&radiance)) { return; }

    let probe = probes[probeIndex];
    let probeValid =
        (probe.identityState.z & SPARSE_VOXEL_PROBE_STATE_VALID) != 0u;
    let historyIndex = probe.pixelFrameHistory.w;
    let hasHistory =
        (probe.identityState.z & SPARSE_VOXEL_PROBE_STATE_REPROJECTED) != 0u;
    // Every lane in the workgroup agrees on this, so the early out is uniform
    // and the barriers below stay well formed.
    if (!probeValid || settings.probeCounts.z == 0u) {
        radiance[cellBase + cellIndex] =
            vec4<f32>(0.0, 0.0, 0.0, SPARSE_VOXEL_PROBE_NO_SAMPLE);
        return;
    }
    // Resolve replaces every far-field cell with deterministic analytical sky
    // and sun. Reprojecting a 3x3 neighbourhood here would perform thousands of
    // threadgroup atomics only to have those values overwritten later.
    if (distance(probe.positionDepth.xyz, settings.cameraPositionCutoff.xyz) >=
        settings.cameraPositionCutoff.w) {
        radiance[cellBase + cellIndex] =
            vec4<f32>(0.0, 0.0, 0.0, SPARSE_VOXEL_PROBE_NO_SAMPLE);
        return;
    }

    let carriedSchedule = hasHistory &&
        historyIndex < arrayLength(&previousSchedule);
    var previous = sparse_voxel_probe_empty_schedule();
    if (carriedSchedule) { previous = previousSchedule[historyIndex]; }

    let probePosition = probe.positionDepth.xyz;
    let probeNormal = probe.normalRadius.xyz;
    // The neighbourhood is centred on the probe this one reprojected from, not
    // on its own tile: a probe whose surface moved across tiles in screen space
    // still wants the samples belonging to that surface patch.
    //
    // A refined probe has no such coordinate - its predecessor lives in the
    // overflow range past the end of the uniform grid - so it centres on its
    // tile instead. Refinement never places a probe outside the tile that
    // spawned it, so that tile's uniform probe and its neighbours describe the
    // same patch of surface. Bailing out here instead, which is what this pass
    // used to do, left every refined probe with no shared history at all: a
    // single-frame 64-ray estimate, no temporal blend, and nothing to fall back
    // on for a cell whose ray came back unresolved.
    // A reprojected probe centres the search on its predecessor. A brand-new
    // probe has no predecessor, but the probes around its *current* screen tile
    // still describe the visible surfaces next to it and are a useful initial
    // lighting prior. This is the path that turns disocclusion from a 64-ray
    // cold start into a neighbour-seeded update.
    var baseCoord = vec2<i32>(
        probe.pixelFrameHistory.xy / max(settings.counts.y, 1u));
    if (hasHistory && historyIndex < uniformCount) {
        baseCoord = vec2<i32>(
            i32(historyIndex % probeCounts.x),
            i32(historyIndex / probeCounts.x));
    } else if (hasHistory && historyIndex >= uniformCount) {
        baseCoord = vec2<i32>(
            probe.pixelFrameHistory.xy / max(settings.counts.y, 1u));
    }
    // Neighbour acceptance is measured against the world footprint the probe
    // serves, so it scales with depth exactly like the interpolation weights.
    let epsilon = max(
        probe.normalRadius.w * max(settings.weights.y, 1.0e-3), 1.0e-4);
    let normalPower = max(settings.weights.x, 1.0);
    let minimumWeight = max(settings.weights.z, 0.0);
    let densityStride = i32(sparse_voxel_probe_density_stride(
        probe.identityState.z));

    // A probe whose updates the scheduler has already slowed down gathers only
    // its own predecessor instead of the whole neighbourhood.
    //
    // Borrowing a neighbour's rays is what gives a *noisy* probe more samples
    // than it traced. A converged probe does not need them: it has its own
    // accumulated history for every direction, and the neighbours only add a
    // smoothing term it has already absorbed over the frames it spent
    // converging. Its own predecessor still reprojects with full parallax, so
    // nothing about the estimate becomes approximate - there is simply one
    // source instead of nine, which is where nine tenths of this pass's scatter
    // and its five atomics per cell per neighbour go.
    let settled = settings.counts.z != 0u && carriedSchedule &&
        sparse_voxel_probe_last_tier(previous.counters) >
        SPARSE_VOXEL_PROBE_TIER_AWAKE && densityStride == 1;
    if (!settled) {
        for (var y = -1; y <= 1; y += 1) {
            for (var x = -1; x <= 1; x += 1) {
                // Distance-LOD probes are farther apart in the screen grid.
                // Step over the invalid tiles between them so their 3x3 share
                // neighbourhood still contains eight live peers.
                let coord = baseCoord +
                    vec2<i32>(x, y) * densityStride;
                if (any(coord < vec2<i32>(0)) ||
                    any(coord >= vec2<i32>(probeCounts))) { continue; }
                probe_share_gather(
                    sparse_voxel_probe_index(vec2<u32>(coord), probeCounts),
                    cellIndex, probePosition, probeNormal, epsilon, normalPower,
                    minimumWeight);
            }
        }
    }
    // The probe's own predecessor. The grid walk above reaches it only when it
    // is a uniform-grid probe, and it is skipped entirely for a settled one, so
    // gather it here whenever the walk did not. It describes this exact
    // surface, so it carries the highest weight of anything gathered - which is
    // also what makes a refined probe accumulate over time rather than
    // restarting from a single frame's rays every frame.
    if (hasHistory && historyIndex < arrayLength(&previousProbes) &&
        (settled || historyIndex >= uniformCount)) {
        probe_share_gather(
            historyIndex, cellIndex, probePosition, probeNormal, epsilon,
            normalPower, minimumWeight);
    }
    workgroupBarrier();

    let weightSum =
        f32(atomicLoad(&binWeight[cellIndex])) / PROBE_SHARE_WEIGHT_SCALE;
    var value = vec3<f32>(0.0);
    var travelled = SPARSE_VOXEL_PROBE_NO_SAMPLE;
    if (weightSum > 1.0e-4) {
        value = vec3<f32>(
            f32(atomicLoad(&binRed[cellIndex])),
            f32(atomicLoad(&binGreen[cellIndex])),
            f32(atomicLoad(&binBlue[cellIndex]))) /
            (PROBE_SHARE_RADIANCE_SCALE * weightSum);
        travelled = min(
            f32(atomicLoad(&binDistance[cellIndex])) /
                PROBE_SHARE_DISTANCE_SCALE,
            SPARSE_VOXEL_PROBE_SKY_DISTANCE);
        atomicAdd(&meanRed, probe_share_quantise(
            value.x, PROBE_SHARE_RADIANCE_SCALE));
        atomicAdd(&meanGreen, probe_share_quantise(
            value.y, PROBE_SHARE_RADIANCE_SCALE));
        atomicAdd(&meanBlue, probe_share_quantise(
            value.z, PROBE_SHARE_RADIANCE_SCALE));
        atomicAdd(&meanCount, 1u);
    }
    workgroupBarrier();

    let count = atomicLoad(&meanCount);
    if (weightSum <= 1.0e-4) {
        // Nothing reprojected into this cell. Reprojection scatters, so with
        // the probe jittering inside its tile a handful of cells receive
        // nothing on any given frame even when the neighbourhood is healthy.
        //
        // Before falling back to the mean, take the probe's own previous value
        // for this same direction. It is not parallax corrected - it is the
        // same probe and the same cell, so there is nothing to correct against
        // beyond a frame of jitter - and crucially it carries a *distance*,
        // which keeps the cell alive as a directional sample.
        //
        // Without this the mean fallback below writes a negative distance, and
        // a cell with no distance cannot be reprojected next frame either. The
        // holes therefore accumulate: harmless when every probe retraced every
        // frame and refilled them, but with update scheduling a probe can go
        // many frames without tracing, and the erosion eats its whole map.
        var filled = false;
        let ownIndex =
            historyIndex * SPARSE_VOXEL_PROBE_DIRECTION_COUNT + cellIndex;
        if (hasHistory && historyIndex < arrayLength(&previousProbes) &&
            ownIndex < arrayLength(&previousRadiance)) {
            let own = previousRadiance[ownIndex];
            // Only where the probe barely moved relative to how far away the
            // sample is. Copying a cell across unchanged *is* a reprojection -
            // the identity one - and it is accurate exactly in the limit where
            // the displacement is small next to the hit distance. Applying it
            // regardless keeps a sample aimed in a direction the probe no
            // longer looks, and because the copy carries a valid distance it
            // becomes a legitimate reprojection source next frame, so the error
            // compounds instead of washing out. That is what shows up as
            // patches of stale sky or stale shadow that survive for seconds.
            //
            // A sky sample has effectively infinite distance and so is always
            // safe; a near hit while the probe slides along a wall never is,
            // and dropping the cell there is correct - the resolve renormalises
            // over the cells that do know something.
            // The normal has to match as well, and for a sky cell it is the
            // *only* thing that matters: a sky sample sits at effectively
            // infinite distance, so the displacement test above can never
            // reject it, and cell c means a different world direction the
            // moment the probe's frame rotates. Without this a stale sky
            // sample is copied forward indefinitely into a direction that may
            // since have become occluded - which is exactly a blue patch that
            // will not go away.
            let previousNormal =
                previousProbes[historyIndex].normalRadius.xyz;
            let alignedFrame = dot(probeNormal, previousNormal) >= 0.999;
            let displacement = length(
                probePosition - previousProbes[historyIndex].positionDepth.xyz);
            if (own.w >= 0.0 && alignedFrame &&
                displacement <= own.w * 0.05) {
                value = own.xyz;
                travelled = own.w;
                filled = true;
            }
        }
        // Cells not even the probe's own history reached still carry its mean
        // shared radiance. It is not a directional sample - the negative
        // distance says so - but it is a far better prior than black for a cell
        // whose own ray comes back unresolved.
        if (!filled && count != 0u) {
            value = vec3<f32>(
                f32(atomicLoad(&meanRed)),
                f32(atomicLoad(&meanGreen)),
                f32(atomicLoad(&meanBlue))) /
                (PROBE_SHARE_RADIANCE_SCALE * f32(count));
        }
    }
    radiance[cellBase + cellIndex] = vec4<f32>(value, travelled);
}
