#include "sparse_voxel_probe_common.wgsl"

// Spatial filtering in radiance cache space, following Lumen.
//
// This is the cheapest noise reduction in the whole path, and the reason is
// entirely about which space it runs in. A probe covers an 8x8 tile, so a 3x3
// neighbourhood of probes spans 24x24 pixels - and because the probes it
// averages are themselves already hemisphere integrals rather than single
// samples, the variance reduction is closer to what a 48x48 screen-space kernel
// would buy. The cost is nine SH loads and nine probe records.
//
// The screen-space equivalent would have to load position and normal for every
// one of those pixels to weight them, then a radiance sample each, and would
// still only be averaging one-sample estimates. Filtering where the data is
// already sparse and already integrated is what makes the kernel affordable.
//
// The weights are the same surface test used everywhere else in this path, for
// the same reason: a neighbour describing a different surface must contribute
// nothing, or the filter becomes the leak. Nine taps of a *correct* filter beat
// a wide one that has to be timid about what it accepts.

struct SparseVoxelProbeFilterSettings {
    // x probes across, y probes down, z total probes, w filter radius in
    // probes.
    probeCounts : vec4<u32>,
    // x normal weight power, y probe radius scale for the plane falloff,
    // z minimum accepted neighbour weight, w unused.
    weights : vec4<f32>,
}

@group(0) @binding(0) var<uniform> settings : SparseVoxelProbeFilterSettings;
@group(0) @binding(1) var<storage, read> probes : array<SparseVoxelProbe>;
@group(0) @binding(2) var<storage, read> sourceSH : array<vec4<f32>>;
@group(0) @binding(3) var<storage, read_write> filteredSH : array<vec4<f32>>;

// One lane per coefficient, nine lanes per probe. The workgroup covers seven
// probes and the remainder idles, which is still far cheaper than a lane per
// probe reloading nine coefficients from every neighbour.
const PROBE_FILTER_GROUP : u32 = 63u;

@compute @workgroup_size(63, 1, 1)
fn filterSparseVoxelProbes(
    @builtin(global_invocation_id) gid : vec3<u32>) {
    let probeIndex = gid.x / SPARSE_VOXEL_PROBE_SH_COUNT;
    let coefficient = gid.x % SPARSE_VOXEL_PROBE_SH_COUNT;
    let probeCounts = settings.probeCounts.xy;
    let uniformCount = probeCounts.x * probeCounts.y;
    let totalCount = max(settings.probeCounts.z, uniformCount);
    if (probeIndex >= totalCount || probeIndex >= arrayLength(&probes)) {
        return;
    }
    let destination = probeIndex * SPARSE_VOXEL_PROBE_SH_COUNT + coefficient;
    if (destination >= arrayLength(&filteredSH)) { return; }

    let probe = probes[probeIndex];
    if ((probe.identityState.z & SPARSE_VOXEL_PROBE_STATE_VALID) == 0u) {
        filteredSH[destination] = vec4<f32>(0.0);
        return;
    }

    var accumulated = sourceSH[destination].xyz;
    var totalWeight = 1.0;

    // The neighbourhood walks the uniform grid. A refined probe filters against
    // the tile grid around the tile that spawned it, which is where its
    // neighbours are; refined probes are too sparse and too short-lived to
    // form a neighbourhood of their own.
    let tileSize = max(settings.probeCounts.w, 1u);
    let anchor = vec2<i32>(
        i32((probe.pixelFrameHistory.x / tileSize)),
        i32((probe.pixelFrameHistory.y / tileSize)));
    let epsilon = max(
        probe.normalRadius.w * max(settings.weights.y, 1.0e-3), 1.0e-4);
    let normalPower = max(settings.weights.x, 1.0);
    let minimumWeight = max(settings.weights.z, 0.0);
    let densityStride = i32(sparse_voxel_probe_density_stride(
        probe.identityState.z));

    for (var y = -1; y <= 1; y += 1) {
        for (var x = -1; x <= 1; x += 1) {
            if (x == 0 && y == 0) { continue; }
            let coord = anchor + vec2<i32>(x, y) * densityStride;
            if (any(coord < vec2<i32>(0)) ||
                any(coord >= vec2<i32>(probeCounts))) { continue; }
            let neighbourIndex = sparse_voxel_probe_index(
                vec2<u32>(coord), probeCounts);
            if (neighbourIndex == probeIndex ||
                neighbourIndex >= arrayLength(&probes)) { continue; }
            let neighbour = probes[neighbourIndex];
            if ((neighbour.identityState.z &
                SPARSE_VOXEL_PROBE_STATE_VALID) == 0u) { continue; }

            // Orientation and distance to this probe's plane, exactly as
            // interpolation weights a probe against a pixel.
            let orientation = pow(
                clamp(
                    dot(probe.normalRadius.xyz, neighbour.normalRadius.xyz),
                    0.0, 1.0),
                normalPower);
            if (orientation <= 0.0) { continue; }
            let offset = neighbour.positionDepth.xyz - probe.positionDepth.xyz;
            let planeDistance = abs(dot(offset, probe.normalRadius.xyz));
            let planeWeight = clamp(1.0 - planeDistance / epsilon, 0.0, 1.0);
            let weight = orientation * planeWeight;
            if (weight <= minimumWeight) { continue; }

            let source =
                neighbourIndex * SPARSE_VOXEL_PROBE_SH_COUNT + coefficient;
            if (source >= arrayLength(&sourceSH)) { continue; }
            accumulated += sourceSH[source].xyz * weight;
            totalWeight += weight;
        }
    }

    filteredSH[destination] = vec4<f32>(
        accumulated / max(totalWeight, 1.0e-4), sourceSH[destination].w);
}
