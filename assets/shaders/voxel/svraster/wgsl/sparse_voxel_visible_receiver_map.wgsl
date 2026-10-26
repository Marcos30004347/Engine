#include "sparse_voxel_types.wgsl"

// Per-visible-voxel screen bounds and five representative pixels. The table is
// rebuilt every frame because projected bounds change with the camera, while
// the exact brick-cell identity keeps unrelated faces from sharing entries.
struct SparseVoxelVisibleReceiverSettings {
    // xy: raster dimensions, z: power-of-two table capacity.
    dimensions : vec4<u32>,
}

struct SparseVoxelVisibleReceiverEntry {
    state : atomic<u32>,
    generation : atomic<u32>,
    identity0 : u32,
    identity1 : u32,
    minimumX : atomic<u32>,
    minimumY : atomic<u32>,
    maximumX : atomic<u32>,
    maximumY : atomic<u32>,
    pixelCount : atomic<u32>,
    representatives : array<atomic<u32>, 5>,
}

@group(0) @binding(0) var<uniform> settings : SparseVoxelVisibleReceiverSettings;
@group(0) @binding(1) var<storage, read> coverage : array<u32>;
@group(0) @binding(2) var instanceIdTexture : texture_2d<u32>;
@group(0) @binding(3) var voxelIdTexture : texture_2d<u32>;
@group(0) @binding(4) var geometryNormalTexture : texture_2d<f32>;
@group(0) @binding(5) var<storage, read_write> entries :
    array<SparseVoxelVisibleReceiverEntry>;

const VISIBLE_RECEIVER_BUSY = 0xfffffffeu;
const VISIBLE_RECEIVER_PIXEL_MASK = 0x00ffffffu;

fn visible_receiver_hash(identity : vec2<u32>) -> u32 {
    var value = identity.x ^ (identity.y * 0x9e3779b9u);
    value = (value ^ (value >> 16u)) * 0x7feb352du;
    value = (value ^ (value >> 15u)) * 0x846ca68bu;
    value = value ^ (value >> 16u);
    if (value == 0u || value == VISIBLE_RECEIVER_BUSY) {
        value = value ^ 0xa511e9b3u;
    }
    return max(value, 1u);
}

fn visible_receiver_identity(pixel : vec2<u32>) -> vec2<u32> {
    let pixelI = vec2<i32>(pixel);
    let normal = textureLoad(geometryNormalTexture, pixelI, 0).xyz;
    let magnitude = abs(normal);
    var axis = 0u;
    if (magnitude.y > magnitude.x) { axis = 1u; }
    if (magnitude.z > max(magnitude.x, magnitude.y)) { axis = 2u; }
    let face = axis | select(0u, 4u, normal[axis] < 0.0);
    let instance = textureLoad(instanceIdTexture, pixelI, 0).x;
    return vec2<u32>(
        (instance & ((1u << 29u) - 1u)) | (face << 29u),
        textureLoad(voxelIdTexture, pixelI, 0).x);
}

fn visible_receiver_same(
    candidate : vec2<i32>, dimensions : vec2<u32>,
    identity : vec2<u32>) -> bool {
    if (any(candidate < vec2<i32>(0)) ||
        any(candidate >= vec2<i32>(dimensions))) { return false; }
    let candidateU = vec2<u32>(candidate);
    let candidateIndex = candidateU.y * dimensions.x + candidateU.x;
    return candidateIndex < arrayLength(&coverage) &&
        coverage[candidateIndex] != 0u &&
        all(visible_receiver_identity(candidateU) == identity);
}

fn visible_receiver_find(identity : vec2<u32>) -> u32 {
    let capacity = min(settings.dimensions.z, arrayLength(&entries));
    if (capacity == 0u) { return 0xffffffffu; }
    let hash = visible_receiver_hash(identity);
    for (var probe = 0u; probe < 8u; probe += 1u) {
        let entryIndex = (hash + probe * probe) & (capacity - 1u);
        if (atomicLoad(&entries[entryIndex].generation) ==
                settings.dimensions.w &&
            atomicLoad(&entries[entryIndex].state) == hash &&
            entries[entryIndex].identity0 == identity.x &&
            entries[entryIndex].identity1 == identity.y) {
            return entryIndex;
        }
    }
    return 0xffffffffu;
}

@compute @workgroup_size(64, 1, 1)
fn buildSparseVoxelVisibleReceiverBounds(
    @builtin(global_invocation_id) gid : vec3<u32>) {
    let dimensions = settings.dimensions.xy;
    let pixelIndex = gid.x;
    if (pixelIndex >= dimensions.x * dimensions.y ||
        pixelIndex >= arrayLength(&coverage) || coverage[pixelIndex] == 0u) {
        return;
    }
    let pixel = vec2<u32>(pixelIndex % dimensions.x, pixelIndex / dimensions.x);
    let identity = visible_receiver_identity(pixel);
    if ((identity.y & SPARSE_VOXEL_TEMPORAL_ID_EXACT) == 0u) { return; }
    let capacity = min(settings.dimensions.z, arrayLength(&entries));
    if (capacity == 0u) { return; }
    let hash = visible_receiver_hash(identity);
    var selected = 0xffffffffu;
    for (var probe = 0u; probe < 8u; probe += 1u) {
        let entryIndex = (hash + probe * probe) & (capacity - 1u);
        let generation = atomicLoad(&entries[entryIndex].generation);
        let state = atomicLoad(&entries[entryIndex].state);
        if (generation == settings.dimensions.w && state == hash &&
            entries[entryIndex].identity0 == identity.x &&
            entries[entryIndex].identity1 == identity.y) {
            selected = entryIndex;
            break;
        }
        if (generation != settings.dimensions.w &&
            generation != VISIBLE_RECEIVER_BUSY) {
            let claim = atomicCompareExchangeWeak(
                &entries[entryIndex].generation,
                generation, VISIBLE_RECEIVER_BUSY);
            if (claim.exchanged) {
                atomicStore(&entries[entryIndex].state, hash);
                entries[entryIndex].identity0 = identity.x;
                entries[entryIndex].identity1 = identity.y;
                atomicStore(&entries[entryIndex].minimumX, pixel.x);
                atomicStore(&entries[entryIndex].minimumY, pixel.y);
                atomicStore(&entries[entryIndex].maximumX, pixel.x);
                atomicStore(&entries[entryIndex].maximumY, pixel.y);
                atomicStore(&entries[entryIndex].pixelCount, 0u);
                for (var sample = 0u; sample < 5u; sample += 1u) {
                    atomicStore(
                        &entries[entryIndex].representatives[sample],
                        0xffffffffu);
                }
                atomicStore(
                    &entries[entryIndex].generation,
                    settings.dimensions.w);
                selected = entryIndex;
                break;
            }
        }
    }
    if (selected == 0xffffffffu) { return; }
    let pixelI = vec2<i32>(pixel);
    // Only silhouette/identity-boundary pixels modify the shared bounds. A
    // large flat face therefore performs O(perimeter), not O(area), atomics.
    if (!visible_receiver_same(
            pixelI + vec2<i32>(-1, 0), dimensions, identity)) {
        atomicMin(&entries[selected].minimumX, pixel.x);
    }
    if (!visible_receiver_same(
            pixelI + vec2<i32>(1, 0), dimensions, identity)) {
        atomicMax(&entries[selected].maximumX, pixel.x);
    }
    if (!visible_receiver_same(
            pixelI + vec2<i32>(0, -1), dimensions, identity)) {
        atomicMin(&entries[selected].minimumY, pixel.y);
    }
    if (!visible_receiver_same(
            pixelI + vec2<i32>(0, 1), dimensions, identity)) {
        atomicMax(&entries[selected].maximumY, pixel.y);
    }
}

@compute @workgroup_size(64, 1, 1)
fn selectSparseVoxelVisibleReceiverSamples(
    @builtin(global_invocation_id) gid : vec3<u32>) {
    let dimensions = settings.dimensions.xy;
    let pixelIndex = gid.x;
    if (pixelIndex >= dimensions.x * dimensions.y ||
        pixelIndex >= VISIBLE_RECEIVER_PIXEL_MASK ||
        pixelIndex >= arrayLength(&coverage) || coverage[pixelIndex] == 0u) {
        return;
    }
    let pixel = vec2<u32>(pixelIndex % dimensions.x, pixelIndex / dimensions.x);
    let identity = visible_receiver_identity(pixel);
    if ((identity.y & SPARSE_VOXEL_TEMPORAL_ID_EXACT) == 0u) { return; }
    let entryIndex = visible_receiver_find(identity);
    if (entryIndex == 0xffffffffu) { return; }
    let minimum = vec2<u32>(
        atomicLoad(&entries[entryIndex].minimumX),
        atomicLoad(&entries[entryIndex].minimumY));
    let maximum = vec2<u32>(
        atomicLoad(&entries[entryIndex].maximumX),
        atomicLoad(&entries[entryIndex].maximumY));
    let center = (minimum + maximum) >> vec2<u32>(1u);
    let targets = array<vec2<u32>, 5>(
        center,
        vec2<u32>(minimum.x, center.y),
        vec2<u32>(maximum.x, center.y),
        vec2<u32>(center.x, minimum.y),
        vec2<u32>(center.x, maximum.y));
    for (var sample = 0u; sample < 5u; sample += 1u) {
        let candidate = select(
            all(abs(vec2<i32>(pixel) - vec2<i32>(center)) <=
                vec2<i32>(1)),
            pixel.x == minimum.x,
            sample == 1u) ||
            (sample == 2u && pixel.x == maximum.x) ||
            (sample == 3u && pixel.y == minimum.y) ||
            (sample == 4u && pixel.y == maximum.y);
        if (!candidate) { continue; }
        let delta = abs(vec2<i32>(pixel) - vec2<i32>(targets[sample]));
        let distance = min(u32(delta.x + delta.y), 255u);
        atomicMin(
            &entries[entryIndex].representatives[sample],
            (distance << 24u) | pixelIndex);
    }
}
