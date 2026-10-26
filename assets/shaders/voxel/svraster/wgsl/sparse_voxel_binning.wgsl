#include "sparse_voxel_types.wgsl"
#include "sparse_voxel_projection.wgsl"

@group(0) @binding(0) var<uniform> uniforms : SparseVoxelRasterUniforms;
@group(0) @binding(1) var<storage, read_write> counters : SparseVoxelFrameCounters;
@group(0) @binding(2) var<storage, read> visibleInstances : array<VisibleInstance>;
@group(0) @binding(3) var<storage, read> instances : array<SparseVoxelInstance>;
@group(0) @binding(4) var<storage, read> centersLengths : array<SparseVoxelCenterLength>;
@group(0) @binding(5) var<storage, read> pathsLevels : array<SparseVoxelPathLevel>;
@group(0) @binding(6) var<storage, read_write> projectedVoxels : array<ProjectedVoxel>;
@group(0) @binding(7) var<storage, read_write> duplicateOffsets : array<u32>;
@group(0) @binding(8) var<storage, read_write> scanBlockSums : array<u32>;
@group(0) @binding(9) var<storage, read_write> scanBlockOffsets : array<u32>;
@group(0) @binding(10) var<storage, read_write> candidates : array<TileCandidate>;
@group(0) @binding(11) var<storage, read_write> tileRanges : array<SparseVoxelTileRange>;
@group(0) @binding(12) var<storage, read_write> indirectArgs : array<IndirectDispatchArgs>;
@group(0) @binding(13) var<storage, read_write> candidateCount : array<u32>;
@group(0) @binding(14) var<storage, read> visibleVoxels : array<VisibleVoxel>;
@group(0) @binding(15) var<storage, read_write> tileCandidateCounts : array<atomic<u32>>;
@group(0) @binding(16) var<storage, read_write> tileCandidateCursors : array<atomic<u32>>;
// Slim tile candidate: one projected-voxel index per (voxel, tile) pair.
//
// Everything the resolver reads out of a bucket entry - instance, voxel,
// quadrant bitset, conservative hit lower bound - already lives in
// projectedVoxels, which the resolver loads anyway for the pixel bounds. So the
// 32-byte TileCandidate the scatter used to write was 28 bytes of duplicate.
@group(0) @binding(17) var<storage, read_write> tileCandidateIndices : array<u32>;

var<workgroup> scanValues : array<u32, 256>;
var<workgroup> scanTotal : u32;

// In-place exclusive Blelloch scan of scanValues, returning the total on every
// lane. The caller populates scanValues and issues the barrier that publishes
// it; every barrier inside is reached by the whole workgroup.
fn scanValuesExclusive(localIndex : u32) -> u32 {
    var offset = 1u;
    for (var activeCount = 128u; activeCount > 0u; activeCount >>= 1u) {
        if (localIndex < activeCount) {
            let ai = offset * (2u * localIndex + 1u) - 1u;
            let bi = offset * (2u * localIndex + 2u) - 1u;
            scanValues[bi] += scanValues[ai];
        }
        offset <<= 1u;
        workgroupBarrier();
    }
    if (localIndex == 0u) {
        scanTotal = scanValues[255u];
        scanValues[255u] = 0u;
    }
    workgroupBarrier();
    for (var activeCount = 1u; activeCount < 256u; activeCount <<= 1u) {
        offset >>= 1u;
        if (localIndex < activeCount) {
            let ai = offset * (2u * localIndex + 1u) - 1u;
            let bi = offset * (2u * localIndex + 2u) - 1u;
            let value = scanValues[ai];
            scanValues[ai] = scanValues[bi];
            scanValues[bi] += value;
        }
        workgroupBarrier();
    }
    return scanTotal;
}

fn zeroDispatch() -> IndirectDispatchArgs {
    return IndirectDispatchArgs(0u, 0u, 0u, 0u);
}

fn dispatchFor(count : u32, workgroupSize : u32) -> IndirectDispatchArgs {
    return IndirectDispatchArgs((count + workgroupSize - 1u) / workgroupSize, 1u, 1u, 0u);
}

fn invalidProjection() -> ProjectedVoxel {
    return ProjectedVoxel(vec2<u32>(0u), 0u, 0u, SPARSE_VOXEL_INVALID_INDEX, SPARSE_VOXEL_INVALID_INDEX, 0u, 0u);
}

fn visibleVoxelOwner(visibleVoxelIndex : u32) -> vec3<u32> {
    let visible = visibleVoxels[visibleVoxelIndex];
    return vec3<u32>(visible.instanceIndex, visible.voxelIndex, 0u);
}

// Every bucket stage - clear, count, prefix, scatter - must agree on which
// tiles participate. A tile that one stage owns and another skips is either a
// bucket that never gets a range or a range nothing ever fills.
fn tileBucketCapacity() -> u32 {
    return min(
        uniforms.tileRangeCapacity,
        min(arrayLength(&tileRanges),
            min(arrayLength(&tileCandidateCounts),
                arrayLength(&tileCandidateCursors))));
}

struct PreprocessOutcome {
    // Inclusive tile rectangle the voxel covers. Meaningful only when emitted.
    tileRect : vec4<u32>,
    emitted : u32,
}

fn noProjection() -> PreprocessOutcome {
    return PreprocessOutcome(vec4<u32>(0u), 0u);
}

fn projectVisibleVoxel(index : u32) -> PreprocessOutcome {
    let visibleVoxelCount = atomicLoad(&counters.visibleVoxelCount);
    if (index >= visibleVoxelCount) {
        return noProjection();
    }
    if (visibleVoxelCount > uniforms.projectedVoxelCapacity) {
        if (index == 0u) {
            atomicStore(&counters.visibleVoxelCount, 0u);
            atomicStore(&counters.candidateCount, 0u);
            atomicStore(&counters.overflowStage, SPARSE_VOXEL_OVERFLOW_PROJECTED_VOXELS);
            atomicStore(&counters.requiredCount, visibleVoxelCount);
            atomicStore(&counters.capacity, uniforms.projectedVoxelCapacity);
            candidateCount[0] = 0u;
            indirectArgs[0u] = zeroDispatch();
            indirectArgs[1u] = zeroDispatch();
            indirectArgs[2u] = zeroDispatch();
            indirectArgs[3u] = zeroDispatch();
        }
        return noProjection();
    }

    projectedVoxels[index] = invalidProjection();
    let owner = visibleVoxelOwner(index);
    if (owner.x == SPARSE_VOXEL_INVALID_INDEX) {
        return noProjection();
    }

    let centerLength = centersLengths[owner.y];
    let instance = instances[owner.x];
    let cameraWorld = uniforms.cameraWorldPositionNear.xyz;
    let cameraLocal = transformPoint(instance.worldToModel, cameraWorld);
    let nearDistance = uniforms.cameraWorldPositionNear.w;

    var coordinateMinimum = vec2<f32>(1.0e30);
    var coordinateMaximum = vec2<f32>(-1.0e30);
    var quadrantBitset = 0u;
    var projectedVertexCount = 0u;
    var cameraCorners : array<vec3<f32>, 8>;
    var worldMinimum = vec3<f32>(1.0e30);
    var worldMaximum = vec3<f32>(-1.0e30);
    let radius = 0.5 * centerLength.length;
    for (var cornerIndex = 0u; cornerIndex < 8u; cornerIndex += 1u) {
        let signs = vec3<f32>(
            select(-1.0, 1.0, (cornerIndex & 4u) != 0u),
            select(-1.0, 1.0, (cornerIndex & 2u) != 0u),
            select(-1.0, 1.0, (cornerIndex & 1u) != 0u));
        let localCorner = centerLength.center + radius * signs;
        let worldCorner = transformPoint(instance.modelToWorld, localCorner);
        worldMinimum = min(worldMinimum, worldCorner);
        worldMaximum = max(worldMaximum, worldCorner);
        let cameraCorner = transformPoint(uniforms.worldToCamera, worldCorner);
        cameraCorners[cornerIndex] = cameraCorner;
        quadrantBitset |= 1u << cameraQuadrant(localCorner, cameraLocal);
        if (cameraCorner.z >= nearDistance) {
            projectedVertexCount += 1u;
            let coordinate = cameraCorner.xy / cameraCorner.z;
            coordinateMinimum = min(coordinateMinimum, coordinate);
            coordinateMaximum = max(coordinateMaximum, coordinate);
        }
    }

    // Clip all 12 OBB edges against z=near. The vertices of the clipped
    // convex box are the surviving corners plus these edge intersections, so
    // their projected extrema form an exact conservative screen rectangle.
    let edgeMasks = array<u32, 3>(4u, 2u, 1u);
    for (var cornerIndex = 0u; cornerIndex < 8u; cornerIndex += 1u) {
        for (var axis = 0u; axis < 3u; axis += 1u) {
            let edgeMask = edgeMasks[axis];
            if ((cornerIndex & edgeMask) != 0u) {
                continue;
            }
            let a = cameraCorners[cornerIndex];
            let b = cameraCorners[cornerIndex | edgeMask];
            let aInside = a.z >= nearDistance;
            let bInside = b.z >= nearDistance;
            if (aInside == bInside) {
                continue;
            }
            let interpolation = (nearDistance - a.z) / (b.z - a.z);
            let intersectionXY = mix(a.xy, b.xy, interpolation);
            let coordinate = intersectionXY / nearDistance;
            projectedVertexCount += 1u;
            coordinateMinimum = min(coordinateMinimum, coordinate);
            coordinateMaximum = max(coordinateMaximum, coordinate);
        }
    }
    if (projectedVertexCount == 0u) {
        return noProjection();
    }

    let centerBias = vec2<f32>(uniforms.focalPrincipal.zw) - vec2<f32>(0.5);
    let pixelMinimumF = uniforms.focalPrincipal.xy * coordinateMinimum + centerBias;
    let pixelMaximumF = uniforms.focalPrincipal.xy * coordinateMaximum + centerBias;
    let viewport = uniforms.viewportTiles.xy;
    if (pixelMinimumF.x > f32(viewport.x) || pixelMinimumF.y > f32(viewport.y) ||
        pixelMaximumF.x < 0.0 || pixelMaximumF.y < 0.0) {
        return noProjection();
    }
    // Opaque solid resolution needs a conservative pixel rectangle. SVRaster's
    // nearest rounding is sufficient for volumetric accumulation but can omit
    // the coplanar voxel at a shared edge and expose an internal face.
    let pixelMinimum = clampedFloorPixel(pixelMinimumF, viewport);
    let pixelMaximum = clampedCeilPixel(pixelMaximumF, viewport);
    if (any(pixelMinimum > pixelMaximum)) {
        return noProjection();
    }

    let packedBounds = vec2<u32>(packPixel(pixelMinimum), packPixel(pixelMaximum));
    let tileRect = tileRectangle(packedBounds, uniforms.viewportTiles.zw);
    let tileCount = (tileRect.z - tileRect.x + 1u) * (tileRect.w - tileRect.y + 1u);
    let duplicateCount = tileCount * countOneBits(quadrantBitset);
    // Distance from the camera to the voxel's world AABB is a conservative
    // lower bound for every ray/OBB hit, including non-uniformly transformed
    // instances. The direct resolver can reject a candidate using this bound
    // before loading matrices and evaluating three slabs.
    let distanceVector = max(
        max(worldMinimum - cameraWorld, vec3<f32>(0.0)),
        cameraWorld - worldMaximum);
    let minimumHitDistance = length(distanceVector);
    projectedVoxels[index] = ProjectedVoxel(
        packedBounds, quadrantBitset, duplicateCount, owner.x, owner.y,
        bitcast<u32>(minimumHitDistance), 0u);
    return PreprocessOutcome(tileRect, 1u);
}

@compute @workgroup_size(256, 1, 1)
fn preprocessVisibleVoxels(@builtin(global_invocation_id) globalId : vec3<u32>) {
    projectVisibleVoxel(globalId.x);
}

// Projection with the bucket count fused in.
//
// The separate count pass re-read the whole 32-byte projected record and
// re-derived the same tile rectangle for every visible voxel, purely to
// increment the same counters this kernel can increment while the rectangle is
// still in registers. The clear must therefore be scheduled before projection.
@compute @workgroup_size(256, 1, 1)
fn preprocessAndCountVisibleVoxels(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let outcome = projectVisibleVoxel(globalId.x);
    if (outcome.emitted == 0u) {
        return;
    }
    let tileCapacity = tileBucketCapacity();
    for (var tileY = outcome.tileRect.y; tileY <= outcome.tileRect.w; tileY += 1u) {
        let rowBase = tileY * uniforms.viewportTiles.z;
        for (var tileX = outcome.tileRect.x; tileX <= outcome.tileRect.z; tileX += 1u) {
            let tile = rowBase + tileX;
            if (tile < tileCapacity) {
                atomicAdd(&tileCandidateCounts[tile], 1u);
            }
        }
    }
}

@compute @workgroup_size(256, 1, 1)
fn scanDuplicateBlocks(
    @builtin(local_invocation_id) localId : vec3<u32>,
    @builtin(workgroup_id) workgroupId : vec3<u32>,
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let count = atomicLoad(&counters.visibleVoxelCount);
    let index = globalId.x;
    scanValues[localId.x] = select(0u, projectedVoxels[index].duplicateCount, index < count);
    workgroupBarrier();

    let blockTotal = scanValuesExclusive(localId.x);
    if (localId.x == 0u) {
        scanBlockSums[workgroupId.x] = blockTotal;
    }
    if (index < count) {
        duplicateOffsets[index] = scanValues[localId.x];
    }
}

@compute @workgroup_size(1, 1, 1)
fn scanDuplicateBlockSums() {
    let count = atomicLoad(&counters.visibleVoxelCount);
    let blockCount = (count + 255u) / 256u;
    var total = 0u;
    for (var block = 0u; block < blockCount; block += 1u) {
        scanBlockOffsets[block] = total;
        total += scanBlockSums[block];
    }
    if (total > uniforms.candidateCapacity) {
        // Keep the prefix that fits instead of failing the entire frame
        // closed. No-LOD views of large assets can legitimately exceed the
        // duplicate budget; rendering a deterministic partial set is a much
        // safer degradation than clearing every indirect dispatch and
        // presenting a blank scene.
        let emitted = uniforms.candidateCapacity;
        atomicStore(&counters.candidateCount, emitted);
        atomicStore(&counters.duplicateCount, total);
        atomicStore(&counters.overflowStage, SPARSE_VOXEL_OVERFLOW_TILE_CANDIDATES);
        atomicStore(&counters.requiredCount, total);
        atomicStore(&counters.capacity, uniforms.candidateCapacity);
        candidateCount[0] = emitted;
        indirectArgs[1u] = dispatchFor(count, 256u);
        indirectArgs[2u] = dispatchFor(emitted, 256u);
        indirectArgs[3u] = dispatchFor(emitted, 256u);
        return;
    }
    atomicStore(&counters.candidateCount, total);
    atomicStore(&counters.duplicateCount, total);
    candidateCount[0] = total;
    indirectArgs[1u] = dispatchFor(count, 256u);
    indirectArgs[2u] = dispatchFor(total, 256u);
    indirectArgs[3u] = dispatchFor(total, 256u);
}

@compute @workgroup_size(256, 1, 1)
fn addDuplicateBlockOffsets(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    let count = atomicLoad(&counters.visibleVoxelCount);
    if (index < count) {
        duplicateOffsets[index] += scanBlockOffsets[index / 256u];
    }
}

fn repeatedQuadrantBits(quadrant : u32) -> vec2<u32> {
    var result = vec2<u32>(0u);
    for (var level = 0u; level < 16u; level += 1u) {
        result.y = (result.y << 3u) | (result.x >> 29u);
        result.x = (result.x << 3u) | (quadrant & 7u);
    }
    return result;
}

@compute @workgroup_size(256, 1, 1)
fn emitTileCandidates(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    let count = atomicLoad(&counters.visibleVoxelCount);
    if (index >= count) {
        return;
    }
    let projected = projectedVoxels[index];
    if (projected.duplicateCount == 0u || atomicLoad(&counters.candidateCount) == 0u) {
        return;
    }
    let tileRect = tileRectangle(projected.packedInclusivePixelBounds, uniforms.viewportTiles.zw);
    let pathLevel = pathsLevels[projected.voxelIndex];
    var output = duplicateOffsets[index];
    for (var quadrant = 0u; quadrant < 8u; quadrant += 1u) {
        if ((projected.quadrantBitset & (1u << quadrant)) == 0u) {
            continue;
        }
        let repeated = repeatedQuadrantBits(quadrant);
        let rankLo = pathLevel.pathLo ^ repeated.x;
        let rankHi = (pathLevel.pathHi ^ repeated.y) & 0xffffu;
        for (var tileY = tileRect.y; tileY <= tileRect.w; tileY += 1u) {
            for (var tileX = tileRect.x; tileX <= tileRect.z; tileX += 1u) {
                if (output >= uniforms.candidateCapacity) {
                    return;
                }
                let tileId = tileY * uniforms.viewportTiles.z + tileX;
                candidates[output] = TileCandidate(rankLo, (tileId << 16u) | rankHi, projected.instanceIndex, projected.voxelIndex, quadrant, index, output, 0u);
                output += 1u;
            }
        }
    }
}

@compute @workgroup_size(256, 1, 1)
fn clearTileRanges(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let tile = globalId.x;
    if (tile < uniforms.tileRangeCapacity) {
        tileRanges[tile] = SparseVoxelTileRange(0u, 0u, 0u, 0u);
    }
}

@compute @workgroup_size(256, 1, 1)
fn buildTileRanges(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    let count = atomicLoad(&counters.candidateCount);
    if (index >= count) {
        return;
    }
    let tile = candidates[index].keyHi >> 16u;
    if (tile >= uniforms.tileRangeCapacity) {
        atomicStore(&counters.overflowStage, SPARSE_VOXEL_OVERFLOW_TILE_RANGES);
        atomicMax(&counters.requiredCount, tile + 1u);
        atomicStore(&counters.capacity, uniforms.tileRangeCapacity);
        return;
    }
    if (index == 0u || (candidates[index - 1u].keyHi >> 16u) != tile) {
        tileRanges[tile].begin = index;
    }
    if (index + 1u == count || (candidates[index + 1u].keyHi >> 16u) != tile) {
        tileRanges[tile].end = index + 1u;
    }
}

// Direct tile bucketing path.
//
// The resolver only needs candidates grouped by tile; hitPrecedes() supplies
// the deterministic total order inside each tile. Counting, prefixing and
// scattering into tile buckets therefore replaces the global radix sort and
// its full-candidate scratch copy.

@compute @workgroup_size(256, 1, 1)
fn clearTileBuckets(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let tile = globalId.x;
    if (tile < tileBucketCapacity()) {
        atomicStore(&tileCandidateCounts[tile], 0u);
        atomicStore(&tileCandidateCursors[tile], 0u);
        tileRanges[tile] = SparseVoxelTileRange(0u, 0u, 0u, 0u);
    }
}

@compute @workgroup_size(256, 1, 1)
fn countTileCandidates(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    let count = atomicLoad(&counters.visibleVoxelCount);
    if (index >= count) {
        return;
    }
    let projected = projectedVoxels[index];
    if (projected.duplicateCount == 0u) {
        return;
    }
    let tileRect = tileRectangle(
        projected.packedInclusivePixelBounds, uniforms.viewportTiles.zw);
    let tileCapacity = tileBucketCapacity();
    for (var tileY = tileRect.y; tileY <= tileRect.w; tileY += 1u) {
        for (var tileX = tileRect.x; tileX <= tileRect.z; tileX += 1u) {
            let tile = tileY * uniforms.viewportTiles.z + tileX;
            if (tile < tileCapacity) {
                atomicAdd(&tileCandidateCounts[tile], 1u);
            }
        }
    }
}

// Tile bucket prefix, as a three-kernel parallel scan.
//
// The scan is over one entry per screen tile, and at 1080p with four-pixel
// tiles that is 129600 entries. A single-lane serial prefix over them is a
// full pipeline stall between counting and scattering, twice per frame.
//
// Ranges are clamped to the candidate capacity exactly as the serial prefix
// clamped them: an overflowing frame keeps the deterministic prefix that fits
// rather than failing the whole frame closed.

@compute @workgroup_size(256, 1, 1)
fn scanTileBucketBlocks(
    @builtin(local_invocation_id) localId : vec3<u32>,
    @builtin(workgroup_id) workgroupId : vec3<u32>,
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let tileCapacity = tileBucketCapacity();
    let tile = globalId.x;
    let count = select(
        0u, atomicLoad(&tileCandidateCounts[tile]), tile < tileCapacity);
    scanValues[localId.x] = count;
    workgroupBarrier();

    let blockTotal = scanValuesExclusive(localId.x);
    if (localId.x == 0u && workgroupId.x < arrayLength(&scanBlockSums)) {
        scanBlockSums[workgroupId.x] = blockTotal;
    }
    if (tile < tileCapacity) {
        // Block-local for now; addTileBucketBlockOffsets adds the block base.
        let begin = scanValues[localId.x];
        tileRanges[tile] = SparseVoxelTileRange(begin, begin + count, 0u, 0u);
    }
}

@compute @workgroup_size(256, 1, 1)
fn scanTileBucketBlockSums(
    @builtin(local_invocation_id) localId : vec3<u32>) {
    let tileCapacity = tileBucketCapacity();
    let blockCount = (tileCapacity + 255u) / 256u;
    let blockCapacity = arrayLength(&scanBlockOffsets);
    var total = 0u;
    for (var base = 0u; base < blockCount; base += 256u) {
        let block = base + localId.x;
        scanValues[localId.x] = select(
            0u, scanBlockSums[block],
            block < blockCount && block < arrayLength(&scanBlockSums));
        workgroupBarrier();

        let chunkTotal = scanValuesExclusive(localId.x);
        if (block < blockCount && block < blockCapacity) {
            scanBlockOffsets[block] = total + scanValues[localId.x];
        }
        total += chunkTotal;
        workgroupBarrier();
    }
    if (localId.x == 0u) {
        let emitted = min(total, uniforms.candidateCapacity);
        atomicStore(&counters.candidateCount, emitted);
        atomicStore(&counters.duplicateCount, total);
        candidateCount[0] = emitted;
        if (total > uniforms.candidateCapacity) {
            atomicStore(
                &counters.overflowStage,
                SPARSE_VOXEL_OVERFLOW_TILE_CANDIDATES);
            atomicStore(&counters.requiredCount, total);
            atomicStore(&counters.capacity, uniforms.candidateCapacity);
        }
    }
}

@compute @workgroup_size(256, 1, 1)
fn addTileBucketBlockOffsets(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let tile = globalId.x;
    if (tile >= tileBucketCapacity()) {
        return;
    }
    let block = tile / 256u;
    if (block >= arrayLength(&scanBlockOffsets)) {
        return;
    }
    let blockOffset = scanBlockOffsets[block];
    let range = tileRanges[tile];
    tileRanges[tile] = SparseVoxelTileRange(
        min(range.begin + blockOffset, uniforms.candidateCapacity),
        min(range.end + blockOffset, uniforms.candidateCapacity),
        0u, 0u);
}

@compute @workgroup_size(256, 1, 1)
fn scatterTileCandidates(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    let count = atomicLoad(&counters.visibleVoxelCount);
    if (index >= count) {
        return;
    }
    let projected = projectedVoxels[index];
    if (projected.duplicateCount == 0u) {
        return;
    }
    let tileRect = tileRectangle(
        projected.packedInclusivePixelBounds, uniforms.viewportTiles.zw);
    let tileCapacity = tileBucketCapacity();
    let indexCapacity = arrayLength(&tileCandidateIndices);
    for (var tileY = tileRect.y; tileY <= tileRect.w; tileY += 1u) {
        // The tiles a voxel covers in one row are contiguous, so the row's
        // ranges are loaded once instead of once per tile.
        let rowBase = tileY * uniforms.viewportTiles.z;
        for (var tileX = tileRect.x; tileX <= tileRect.z; tileX += 1u) {
            let tile = rowBase + tileX;
            if (tile >= tileCapacity) {
                continue;
            }
            let range = tileRanges[tile];
            let slot = atomicAdd(&tileCandidateCursors[tile], 1u);
            let output = range.begin + slot;
            if (output < range.end && output < indexCapacity) {
                tileCandidateIndices[output] = index;
            }
        }
    }
}
