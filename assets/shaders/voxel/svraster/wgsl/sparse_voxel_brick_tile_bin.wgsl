// R9 H0: measurement-only screen-tile binning for the visible brick set.
//
// The hybrid raster routes each visible brick to either the hardware queue or a
// tiled compute queue. The compute kernel resolves nearest-wins per pixel with a
// packed atomic, and on this platform that atomic has to be 32-bit: MoltenVK
// reports shaderBufferInt64Atomics = false on an Apple M4, so the Nanite-style
// single 64-bit atomicMin over (depth | payload) is unavailable however well
// Metal itself supports it.
//
// A 32-bit key only works if the payload is a *slot inside the tile's own
// candidate list* rather than a global identifier:
//
//     atomicMin(&tileVisibility[pixel], (distance << SLOT_BITS) | slot)
//
// which spends 20 bits on a distance that only has to order candidates within
// one tile - the exact value is recomputed in the deferred resolve - and 12
// bits on the slot. This pass exists to decide whether 12 bits is enough, by
// counting the candidate list each tile would actually receive. It changes no
// rendering state and writes no attachment.
#include "sparse_voxel_brick_common.wgsl"
#include "sparse_voxel_brick_project.wgsl"

const TILE_SIZE : u32 = 16u;
// Matches the packing above. A tile needing more candidates than this cannot be
// resolved by a 32-bit key and has to fall back to the hardware queue.
const TILE_SLOT_BITS : u32 = 12u;
const TILE_SLOT_CAPACITY : u32 = 1u << TILE_SLOT_BITS;

struct SparseVoxelBrickTileBinStats {
    // Bricks that projected to at least one tile.
    binnedBricks : atomic<u32>,
    // Sum over bricks of the tiles each one touches. This is the size the
    // scatter stage would have to allocate.
    tileReferences : atomic<u32>,
    // Largest tile footprint of any single brick.
    maximumTilesPerBrick : atomic<u32>,
    // Largest candidate list any tile receives. The number that decides
    // TILE_SLOT_BITS.
    maximumCandidatesPerTile : atomic<u32>,
    nonEmptyTiles : atomic<u32>,
    // Tiles whose candidate list exceeds the 32-bit key's slot capacity.
    overflowTiles : atomic<u32>,
    // Candidate-count histogram over nonempty tiles, by power-of-two bucket:
    // 1, 2..3, 4..7, ... 2048..4095, and >= 4096 in the last bucket.
    histogram : array<atomic<u32>, 13>,
    // Bricks rejected before binning because their projection is unusable.
    nearPlaneBricks : atomic<u32>,
    offscreenBricks : atomic<u32>,
    // Candidate writes dropped because the scatter buffer was undersized.
    // Nonzero means the tile lists are incomplete and the compute path must
    // fall back for the frame.
    scatterOverflow : atomic<u32>,
    // Highest slot the scatter claimed, plus one. A correct scan/scatter pair
    // fills exactly [0, tileReferences), so this must equal tileReferences;
    // anything else means the prefix and the cursors disagree.
    scatterHighWater : atomic<u32>,
}

@group(0) @binding(0) var<uniform> uniforms : SparseVoxelBrickCullUniforms;
@group(0) @binding(1) var<storage, read> nodes : array<SparseVoxelBrickNode>;
@group(0) @binding(2) var<storage, read> visibleBricks : array<u32>;
// read_write because SparseVoxelBrickCounters declares atomic members; this
// pass only ever loads from it.
@group(0) @binding(3) var<storage, read_write> counters : SparseVoxelBrickCounters;
@group(0) @binding(4) var<storage, read_write> tileCounts : array<atomic<u32>>;
@group(0) @binding(5) var<storage, read_write> stats : SparseVoxelBrickTileBinStats;
// Exclusive prefix over tileCounts, then reused as the scatter cursor. After
// the scatter it holds each tile's END offset, so a tile's candidate range is
// [tileOffsets[tile] - tileCounts[tile], tileOffsets[tile]).
@group(0) @binding(6) var<storage, read_write> tileOffsets : array<atomic<u32>>;
// Brick node indices grouped by tile. Sized from the H0 census.
@group(0) @binding(7) var<storage, read_write> tileCandidates : array<u32>;
// Phase one's visible list and counters. The hardware raster draws both phases
// into one depth buffer, so a compute path that bins only phase zero is missing
// whatever the second phase recovered and cannot reach parity.
@group(0) @binding(8) var<storage, read> visibleBricks2 : array<u32>;
@group(0) @binding(9) var<storage, read_write> counters2 : SparseVoxelBrickCounters;

fn tileGridDimensions() -> vec2<u32> {
    return (max(uniforms.viewport.xy, vec2<u32>(1u)) + TILE_SIZE - 1u) /
        TILE_SIZE;
}

// Mirrors brickProjectedFootprintPixels() in the draw shader. The two must
// agree exactly or a brick is either drawn twice or not at all.
fn brickProjectedFootprintPixels(node : SparseVoxelBrickNode) -> f32 {
    let bounds = brickOccupiedBoundsCameraRelative(
        node, uniforms.canonicalWorldGrid, uniforms.canonicalCameraCell,
        uniforms.canonicalCameraOffset, uniforms.cameraWorldPosition);
    let closest = clamp(vec3<f32>(0.0), bounds.minimum, bounds.maximum);
    let distance = max(length(closest), 1.0e-4);
    let radius = 0.5 * length(bounds.maximum - bounds.minimum);
    let focal = min(
        abs(uniforms.focalPrincipal.x), abs(uniforms.focalPrincipal.y));
    let projected = radius * focal / distance;
    return projected * projected * 4.0;
}

// Routing uses the draw uniforms' tuning word, which the cull uniforms do not
// carry; the sample mirrors it into the reserved cull slot so both passes read
// one value.
fn brickRoutedToCompute(node : SparseVoxelBrickNode) -> bool {
    return sparseVoxelBrickRoutedToCompute(
        uniforms.routeToComputePixelsBits, brickProjectedFootprintPixels(node));
}

fn visibleBrickCount() -> u32 {
    return min(atomicLoad(&counters.visibleCount), uniforms.visibleCapacity);
}

fn visibleBrickCount2() -> u32 {
    return min(atomicLoad(&counters2.visibleCount), uniforms.visibleCapacity);
}

// Both phases as one logical list, so Count and Scatter agree on indexing.
fn totalVisibleBrickCount() -> u32 {
    return visibleBrickCount() + visibleBrickCount2();
}

fn visibleBrickAt(slot : u32) -> u32 {
    let first = visibleBrickCount();
    if (slot < first) {
        if (slot >= arrayLength(&visibleBricks)) {
            return SPARSE_VOXEL_BRICK_INVALID_INDEX;
        }
        return visibleBricks[slot];
    }
    let second = slot - first;
    if (second >= arrayLength(&visibleBricks2)) {
        return SPARSE_VOXEL_BRICK_INVALID_INDEX;
    }
    return visibleBricks2[second];
}

@compute @workgroup_size(64, 1, 1)
fn resetBrickTileBins(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    let grid = tileGridDimensions();
    let tileCount = grid.x * grid.y;
    if (index == 0u) {
        atomicStore(&stats.binnedBricks, 0u);
        atomicStore(&stats.tileReferences, 0u);
        atomicStore(&stats.maximumTilesPerBrick, 0u);
        atomicStore(&stats.maximumCandidatesPerTile, 0u);
        atomicStore(&stats.nonEmptyTiles, 0u);
        atomicStore(&stats.overflowTiles, 0u);
        atomicStore(&stats.nearPlaneBricks, 0u);
        atomicStore(&stats.offscreenBricks, 0u);
        atomicStore(&stats.scatterOverflow, 0u);
        atomicStore(&stats.scatterHighWater, 0u);
        for (var bucket = 0u; bucket < 13u; bucket += 1u) {
            atomicStore(&stats.histogram[bucket], 0u);
        }
    }
    if (index < tileCount && index < arrayLength(&tileCounts)) {
        atomicStore(&tileCounts[index], 0u);
    }
}

// One thread per visible brick. Projects the tight occupied box - the same box
// the hardware proxy rasterizes - and accumulates its tile footprint.
@compute @workgroup_size(64, 1, 1)
fn countBrickTileBins(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let slot = globalId.x;
    if (slot >= totalVisibleBrickCount()) {
        return;
    }
    let nodeIndex = visibleBrickAt(slot);
    if (nodeIndex >= uniforms.nodeCount || nodeIndex >= arrayLength(&nodes)) {
        return;
    }
    let node = nodes[nodeIndex];
    // When routing is off this is always false and the census covers every
    // visible brick, which is what H0/H1 measure against.
    if (uniforms.routeToComputePixelsBits != 0u && !brickRoutedToCompute(node)) {
        return;
    }
    let bounds = brickOccupiedBoundsCameraRelative(
        node, uniforms.canonicalWorldGrid, uniforms.canonicalCameraCell,
        uniforms.canonicalCameraOffset, uniforms.cameraWorldPosition);
    let projection = brickProjectCameraRelative(
        bounds, uniforms.worldToClip, uniforms.cameraWorldPosition.w);
    if (projection.crossesNearPlane) {
        // A box straddling the eye has no usable screen rectangle. The hybrid
        // router has to send these to the hardware queue, so count them rather
        // than silently dropping them from the census.
        atomicAdd(&stats.nearPlaneBricks, 1u);
        return;
    }
    let viewport = vec2<f32>(max(uniforms.viewport.xy, vec2<u32>(1u)));
    let minimumPixel = clamp(
        projection.screenMinimum * viewport, vec2<f32>(0.0), viewport);
    let maximumPixel = clamp(
        projection.screenMaximum * viewport, vec2<f32>(0.0), viewport);
    if (any(maximumPixel <= minimumPixel)) {
        atomicAdd(&stats.offscreenBricks, 1u);
        return;
    }
    let grid = tileGridDimensions();
    let firstTile = min(vec2<u32>(minimumPixel) / TILE_SIZE, grid - 1u);
    // The maximum pixel is exclusive, so a box ending exactly on a tile
    // boundary must not claim the next tile.
    let lastTile = min(
        vec2<u32>(max(maximumPixel - vec2<f32>(1.0), minimumPixel)) / TILE_SIZE,
        grid - 1u);
    let span = lastTile - firstTile + 1u;
    let touched = span.x * span.y;
    atomicAdd(&stats.binnedBricks, 1u);
    atomicAdd(&stats.tileReferences, touched);
    atomicMax(&stats.maximumTilesPerBrick, touched);
    for (var tileY = firstTile.y; tileY <= lastTile.y; tileY += 1u) {
        for (var tileX = firstTile.x; tileX <= lastTile.x; tileX += 1u) {
            let tile = tileY * grid.x + tileX;
            if (tile < arrayLength(&tileCounts)) {
                atomicAdd(&tileCounts[tile], 1u);
            }
        }
    }
}

// One thread per tile. Reduces the per-tile candidate counts into the summary
// the slot-bit decision actually needs.
@compute @workgroup_size(64, 1, 1)
fn reduceBrickTileBins(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let tile = globalId.x;
    let grid = tileGridDimensions();
    if (tile >= grid.x * grid.y || tile >= arrayLength(&tileCounts)) {
        return;
    }
    let count = atomicLoad(&tileCounts[tile]);
    if (count == 0u) {
        return;
    }
    atomicAdd(&stats.nonEmptyTiles, 1u);
    atomicMax(&stats.maximumCandidatesPerTile, count);
    if (count >= TILE_SLOT_CAPACITY) {
        atomicAdd(&stats.overflowTiles, 1u);
    }
    // firstLeadingBit(1) = 0, so bucket b holds counts in [2^b, 2^(b+1)).
    let bucket = min(firstLeadingBit(count), 12u);
    atomicAdd(&stats.histogram[bucket], 1u);
}

// Single-workgroup blocked exclusive scan over the tile counts. The grid is a
// few thousand entries at any sane resolution, so one workgroup walking chunks
// beats a multi-pass scan and its dispatch overhead.
const TILE_SCAN_THREADS : u32 = 256u;
var<workgroup> tileScanPartials : array<u32, TILE_SCAN_THREADS>;

@compute @workgroup_size(256, 1, 1)
fn scanBrickTileBins(@builtin(local_invocation_id) localId : vec3<u32>) {
    let lane = localId.x;
    let grid = tileGridDimensions();
    let tileCount = min(grid.x * grid.y, arrayLength(&tileCounts));
    let chunk = (tileCount + TILE_SCAN_THREADS - 1u) / TILE_SCAN_THREADS;
    let begin = min(lane * chunk, tileCount);
    let end = min(begin + chunk, tileCount);

    var localSum = 0u;
    for (var tile = begin; tile < end; tile += 1u) {
        localSum += atomicLoad(&tileCounts[tile]);
    }
    tileScanPartials[lane] = localSum;
    workgroupBarrier();

    // Serial scan of 256 partials on one lane. Cheaper than a Hillis-Steele
    // ladder at this width and trivially correct.
    if (lane == 0u) {
        var running = 0u;
        for (var index = 0u; index < TILE_SCAN_THREADS; index += 1u) {
            let value = tileScanPartials[index];
            tileScanPartials[index] = running;
            running += value;
        }
    }
    workgroupBarrier();

    var cursor = tileScanPartials[lane];
    for (var tile = begin; tile < end; tile += 1u) {
        atomicStore(&tileOffsets[tile], cursor);
        cursor += atomicLoad(&tileCounts[tile]);
    }
}

// Second pass over the visible bricks, writing each into every tile it touches.
// atomicAdd on the tile's cursor claims a slot, so tileOffsets ends holding the
// tile's exclusive end. Order inside a tile is arbitrary, which is fine: the
// tile kernel resolves nearest-wins by a depth key, not by list order.
@compute @workgroup_size(64, 1, 1)
fn scatterBrickTileBins(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let slot = globalId.x;
    if (slot >= totalVisibleBrickCount()) {
        return;
    }
    let nodeIndex = visibleBrickAt(slot);
    if (nodeIndex >= uniforms.nodeCount || nodeIndex >= arrayLength(&nodes)) {
        return;
    }
    let node = nodes[nodeIndex];
    if (uniforms.routeToComputePixelsBits != 0u && !brickRoutedToCompute(node)) {
        return;
    }
    let bounds = brickOccupiedBoundsCameraRelative(
        node, uniforms.canonicalWorldGrid, uniforms.canonicalCameraCell,
        uniforms.canonicalCameraOffset, uniforms.cameraWorldPosition);
    let projection = brickProjectCameraRelative(
        bounds, uniforms.worldToClip, uniforms.cameraWorldPosition.w);
    if (projection.crossesNearPlane) {
        return;
    }
    let viewport = vec2<f32>(max(uniforms.viewport.xy, vec2<u32>(1u)));
    let minimumPixel = clamp(
        projection.screenMinimum * viewport, vec2<f32>(0.0), viewport);
    let maximumPixel = clamp(
        projection.screenMaximum * viewport, vec2<f32>(0.0), viewport);
    if (any(maximumPixel <= minimumPixel)) {
        return;
    }
    let grid = tileGridDimensions();
    let firstTile = min(vec2<u32>(minimumPixel) / TILE_SIZE, grid - 1u);
    let lastTile = min(
        vec2<u32>(max(maximumPixel - vec2<f32>(1.0), minimumPixel)) / TILE_SIZE,
        grid - 1u);
    for (var tileY = firstTile.y; tileY <= lastTile.y; tileY += 1u) {
        for (var tileX = firstTile.x; tileX <= lastTile.x; tileX += 1u) {
            let tile = tileY * grid.x + tileX;
            if (tile >= arrayLength(&tileOffsets)) {
                continue;
            }
            let destination = atomicAdd(&tileOffsets[tile], 1u);
            atomicMax(&stats.scatterHighWater, destination + 1u);
            if (destination < arrayLength(&tileCandidates)) {
                tileCandidates[destination] = nodeIndex;
            } else {
                atomicAdd(&stats.scatterOverflow, 1u);
            }
        }
    }
}
