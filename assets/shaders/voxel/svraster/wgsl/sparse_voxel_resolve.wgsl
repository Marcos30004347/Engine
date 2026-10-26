#include "sparse_voxel_types.wgsl"
#include "sparse_voxel_projection.wgsl"

@group(0) @binding(0) var<uniform> uniforms : SparseVoxelRasterUniforms;
@group(0) @binding(1) var<storage, read> instances : array<SparseVoxelInstance>;
@group(0) @binding(2) var<storage, read> centersLengths : array<SparseVoxelCenterLength>;
@group(0) @binding(3) var<storage, read> projectedVoxels : array<ProjectedVoxel>;
@group(0) @binding(4) var<storage, read> candidates : array<TileCandidate>;
@group(0) @binding(5) var<storage, read> tileRanges : array<SparseVoxelTileRange>;
@group(0) @binding(6) var<storage, read_write> resolvedSurfaces : array<ResolvedVoxelSurface>;
@group(0) @binding(7) var<storage, read_write> coverage : array<u32>;
@group(0) @binding(8) var<storage, read_write> linearDepth : array<f32>;
// x: minimum projected area, in pixels, a candidate must cover to be
// considered. Zero considers everything.
// y: non-zero to read slim bucket entries from candidateIndices instead of
// full TileCandidate records. Uniform for the whole dispatch.
@group(0) @binding(9) var<uniform> resolveControls : vec4<u32>;
// Slim bucket entries: one projected-voxel index per candidate. Every field the
// legacy TileCandidate carried is already in projectedVoxels, which is loaded
// for the pixel bounds regardless, so this reads 36 bytes per candidate where
// the record path reads 64.
@group(0) @binding(10) var<storage, read> candidateIndices : array<u32>;
@group(0) @binding(11) var<storage, read_write> materialSampleAddresses :
    array<VoxelMaterialSampleAddress>;


// One 8x8 workgroup resolves multiple fine tile buckets. Each bucket stages
// its own candidate batch, retaining full-wave occupancy without making all 64
// pixels scan the union of the finer buckets.
const RESOLVE_WORKGROUP_PIXELS : u32 = 8u;
const RESOLVE_BUCKETS_PER_AXIS : u32 =
    RESOLVE_WORKGROUP_PIXELS / SPARSE_VOXEL_TILE_SIZE;
const RESOLVE_CANDIDATE_BATCH_SIZE : u32 = SPARSE_VOXEL_TILE_SIZE * SPARSE_VOXEL_TILE_SIZE;
const RESOLVE_SHARED_CANDIDATE_COUNT : u32 =
    RESOLVE_CANDIDATE_BATCH_SIZE *
    RESOLVE_BUCKETS_PER_AXIS * RESOLVE_BUCKETS_PER_AXIS;
var<workgroup> sharedCandidateMetadata : array<vec4<u32>, RESOLVE_SHARED_CANDIDATE_COUNT>;
var<workgroup> sharedMinimumHitDistance : array<u32, RESOLVE_SHARED_CANDIDATE_COUNT>;
var<workgroup> sharedPackedBounds : array<vec2<u32>, RESOLVE_SHARED_CANDIDATE_COUNT>;
// The ULP-padded voxel box, not the center/length it is derived from. The
// padding depends only on the voxel, so staging it here computes it once per
// candidate instead of once per candidate per pixel in the tile.
var<workgroup> sharedPaddedMinimum : array<vec4<f32>, RESOLVE_SHARED_CANDIDATE_COUNT>;
var<workgroup> sharedPaddedMaximum : array<vec4<f32>, RESOLVE_SHARED_CANDIDATE_COUNT>;
// Per-bucket batch counts, so the workgroup-wide batch bound is read from four
// tile ranges rather than re-read from memory by all 64 lanes.
var<workgroup> sharedBucketBatchCount :
    array<u32, RESOLVE_BUCKETS_PER_AXIS * RESOLVE_BUCKETS_PER_AXIS>;

struct VoxelSegment {
    tEnter : f32,
    tExit : f32,
    enterFace : u32,
    exitFace : u32,
    hit : u32,
    cameraInside : u32,
}

fn missSurface() -> ResolvedVoxelSurface {
    return ResolvedVoxelSurface(0.0, SPARSE_VOXEL_INVALID_INDEX, SPARSE_VOXEL_INVALID_INDEX, 0u, 0u, 0u, 0u, 0u);
}

fn packLocalFace(axis : u32, positive : bool) -> u32 {
    return axis | select(0u, 4u, positive);
}

fn rayQuadrant(direction : vec3<f32>) -> u32 {
    return select(0u, 4u, direction.x < 0.0) |
           select(0u, 2u, direction.y < 0.0) |
           select(0u, 1u, direction.z < 0.0);
}

fn pixelCovered(pixel : vec2<u32>, packedBounds : vec2<u32>) -> bool {
    let minimum = unpackPixel(packedBounds.x);
    let maximum = unpackPixel(packedBounds.y);
    return all(pixel >= minimum) && all(pixel <= maximum);
}

// Screen-space area of a candidate's projected bounds.
fn candidatePixelArea(packedBounds : vec2<u32>) -> u32 {
    let minimum = unpackPixel(packedBounds.x);
    let maximum = unpackPixel(packedBounds.y);
    if (any(maximum < minimum)) { return 0u; }
    let extent = maximum - minimum + vec2<u32>(1u);
    return extent.x * extent.y;
}

fn nextFloatDown(value : f32) -> f32 {
    if (value == 0.0) {
        return bitcast<f32>(0x80000001u);
    }
    let bits = bitcast<u32>(value);
    return bitcast<f32>(bits + select(1u, 0xffffffffu, value > 0.0));
}

fn nextFloatUp(value : f32) -> f32 {
    if (value == 0.0) {
        return bitcast<f32>(0x00000001u);
    }
    let bits = bitcast<u32>(value);
    return bitcast<f32>(bits + select(0xffffffffu, 1u, value > 0.0));
}

// Centers and lengths are reconstructed independently from the octree path.
// For non-power-of-two voxel sizes, center +/- radius on two adjacent nodes can
// differ by one or two representable floats. Expanding each endpoint by one ULP
// makes adjacent solid voxels form a watertight union without a world-scale
// epsilon. The result depends only on the voxel, so it is computed once at
// staging time and shared by every pixel in the tile.
fn paddedVoxelMinimum(voxel : SparseVoxelCenterLength) -> vec3<f32> {
    let unpadded = voxel.center - vec3<f32>(0.5 * voxel.length);
    return vec3<f32>(
        nextFloatDown(unpadded.x),
        nextFloatDown(unpadded.y),
        nextFloatDown(unpadded.z));
}

fn paddedVoxelMaximum(voxel : SparseVoxelCenterLength) -> vec3<f32> {
    let unpadded = voxel.center + vec3<f32>(0.5 * voxel.length);
    return vec3<f32>(
        nextFloatUp(unpadded.x),
        nextFloatUp(unpadded.y),
        nextFloatUp(unpadded.z));
}

fn voxelSegmentForPixel(
    origin : vec3<f32>,
    direction : vec3<f32>,
    minimum : vec3<f32>,
    maximum : vec3<f32>,
    maximumT : f32
) -> VoxelSegment {
    let inside = all(origin >= minimum) && all(origin <= maximum);
    var tEnter = -3.402823466e+38;
    var tExit = 3.402823466e+38;
    var enterFace = 0u;
    var exitFace = 0u;
    var enterFaceScore = -1.0;
    var exitFaceScore = -1.0;

    for (var axis = 0u; axis < 3u; axis += 1u) {
        let rayOrigin = origin[axis];
        let rayDirection = direction[axis];
        if (abs(rayDirection) <= 1.0e-20) {
            if (rayOrigin < minimum[axis] || rayOrigin > maximum[axis]) {
                return VoxelSegment(0.0, 0.0, 0u, 0u, 0u, 0u);
            }
            continue;
        }

        let t0 = (minimum[axis] - rayOrigin) / rayDirection;
        let t1 = (maximum[axis] - rayOrigin) / rayDirection;
        let nearT = min(t0, t1);
        let farT = max(t0, t1);
        let faceScore = abs(rayDirection);
        if (enterFaceScore < 0.0) {
            tEnter = nearT;
            enterFace = packLocalFace(axis, rayDirection < 0.0);
            enterFaceScore = faceScore;
        } else {
            let enterTolerance = SPARSE_VOXEL_SURFACE_TIE_EPSILON * max(1.0, max(abs(nearT), abs(tEnter)));
            let scoreTolerance = SPARSE_VOXEL_SURFACE_TIE_EPSILON * max(1.0, max(faceScore, enterFaceScore));
            if (nearT > tEnter + enterTolerance ||
                (abs(nearT - tEnter) <= enterTolerance && faceScore > enterFaceScore + scoreTolerance)) {
                enterFace = packLocalFace(axis, rayDirection < 0.0);
                enterFaceScore = faceScore;
            }
            tEnter = max(tEnter, nearT);
        }
        // Slab entry distance can only increase as more axes are intersected.
        // Once it is beyond the nearest surface already found for this pixel,
        // the candidate cannot win and the remaining divisions/tie handling
        // are pure overdraw. Camera-inside candidates resolve from tExit and
        // therefore cannot use this rejection.
        if (!inside && tEnter > maximumT) {
            // Preserve the same quantized-depth tie group used by
            // hitPrecedes(); a slightly farther shared face may still win its
            // deterministic instance/face/voxel tie break.
            let entryKey =
                (bitcast<u32>(max(tEnter, 0.0)) + 128u) >> 8u;
            let maximumKey =
                (bitcast<u32>(max(maximumT, 0.0)) + 128u) >> 8u;
            if (entryKey > maximumKey) {
                return VoxelSegment(0.0, 0.0, 0u, 0u, 0u, 0u);
            }
        }
        if (exitFaceScore < 0.0) {
            tExit = farT;
            exitFace = packLocalFace(axis, rayDirection > 0.0);
            exitFaceScore = faceScore;
        } else {
            let exitTolerance = SPARSE_VOXEL_SURFACE_TIE_EPSILON * max(1.0, max(abs(farT), abs(tExit)));
            let scoreTolerance = SPARSE_VOXEL_SURFACE_TIE_EPSILON * max(1.0, max(faceScore, exitFaceScore));
            if (farT < tExit - exitTolerance ||
                (abs(farT - tExit) <= exitTolerance && faceScore > exitFaceScore + scoreTolerance)) {
                exitFace = packLocalFace(axis, rayDirection > 0.0);
                exitFaceScore = faceScore;
            }
            tExit = min(tExit, farT);
        }
    }

    if (tEnter > tExit || tExit < 0.0) {
        return VoxelSegment(0.0, 0.0, 0u, 0u, 0u, 0u);
    }
    let surfaceT = select(tEnter, tExit, inside);
    if (surfaceT < 0.0) {
        return VoxelSegment(0.0, 0.0, 0u, 0u, 0u, 0u);
    }
    return VoxelSegment(tEnter, tExit, enterFace, exitFace, 1u, select(0u, 1u, inside));
}

fn hitPrecedes(
    candidateT : f32,
    candidateInstance : u32,
    candidateVoxel : u32,
    candidateFaceScore : f32,
    bestT : f32,
    bestInstance : u32,
    bestVoxel : u32,
    bestFaceScore : f32) -> bool {
    if (bestVoxel == SPARSE_VOXEL_INVALID_INDEX) {
        return true;
    }
    // Positive IEEE-754 floats preserve numeric ordering in their bit
    // representation. Rounded 256-ULP depth buckets keep independently
    // reconstructed shared faces in the same tie group while, unlike pairwise
    // epsilon comparisons, defining a transitive total order. Resolve is
    // therefore independent of the atomic tile-scatter order and cannot
    // flicker when workgroup scheduling changes.
    let candidateTKey =
        (bitcast<u32>(max(candidateT, 0.0)) + 128u) >> 8u;
    let bestTKey = (bitcast<u32>(max(bestT, 0.0)) + 128u) >> 8u;
    if (candidateTKey < bestTKey) {
        return true;
    }
    if (candidateTKey > bestTKey) {
        return false;
    }
    if (candidateInstance != bestInstance) {
        return candidateInstance < bestInstance;
    }
    // Face scores retain a finer 128-ULP bucket.
    let candidateScoreKey =
        (bitcast<u32>(max(candidateFaceScore, 0.0)) + 64u) >> 7u;
    let bestScoreKey =
        (bitcast<u32>(max(bestFaceScore, 0.0)) + 64u) >> 7u;
    if (candidateScoreKey != bestScoreKey) {
        return candidateScoreKey > bestScoreKey;
    }
    return candidateVoxel < bestVoxel;
}

@compute @workgroup_size(8, 8, 1)
fn resolveSolidVoxels(
    @builtin(workgroup_id) workgroupId : vec3<u32>,
    @builtin(local_invocation_id) localId : vec3<u32>) {
    let bucketInWorkgroup = localId.xy / vec2<u32>(SPARSE_VOXEL_TILE_SIZE);
    let pixelInBucket = localId.xy % vec2<u32>(SPARSE_VOXEL_TILE_SIZE);
    let bucket = workgroupId.xy * vec2<u32>(RESOLVE_BUCKETS_PER_AXIS) +
        bucketInWorkgroup;
    let bucketValid = all(bucket < uniforms.viewportTiles.zw);
    let pixel = bucket * vec2<u32>(SPARSE_VOXEL_TILE_SIZE) + pixelInBucket;
    let viewport = uniforms.viewportTiles.xy;
    let insideViewport = bucketValid && all(pixel < viewport);
    var pixelIndex = 0u;
    var worldDirection = vec3<f32>(0.0, 0.0, 1.0);
    if (insideViewport) {
        pixelIndex = pixel.y * viewport.x + pixel.x;
        resolvedSurfaces[pixelIndex] = missSurface();
        coverage[pixelIndex] = 0u;
        linearDepth[pixelIndex] = 0.0;
        materialSampleAddresses[pixelIndex] =
            VoxelMaterialSampleAddress(vec3<i32>(0), 0u);

        let cameraDirection = vec3<f32>(
            (f32(pixel.x) + 0.5 - uniforms.focalPrincipal.z) / uniforms.focalPrincipal.x,
            (f32(pixel.y) + 0.5 - uniforms.focalPrincipal.w) / uniforms.focalPrincipal.y,
            1.0);
        worldDirection = normalize(transformVector(uniforms.cameraToWorld, cameraDirection));
    }
    let worldOrigin = uniforms.cameraWorldPositionNear.xyz;
    var range = SparseVoxelTileRange(0u, 0u, 0u, 0u);
    if (bucketValid) {
        let tileIndex = bucket.y * uniforms.viewportTiles.z + bucket.x;
        range = tileRanges[tileIndex];
    }
    let localIndex =
        pixelInBucket.y * SPARSE_VOXEL_TILE_SIZE + pixelInBucket.x;
    let bucketIndex =
        bucketInWorkgroup.y * RESOLVE_BUCKETS_PER_AXIS +
        bucketInWorkgroup.x;
    let sharedBase = bucketIndex * RESOLVE_CANDIDATE_BATCH_SIZE;
    let candidateCount = range.end - range.begin;
    let batchCount = (candidateCount + RESOLVE_CANDIDATE_BATCH_SIZE - 1u) / RESOLVE_CANDIDATE_BATCH_SIZE;
    // Barriers are workgroup-wide, so every quadrant iterates to the largest
    // bucket batch count even though it only loads and consumes its own range.
    // One lane per bucket publishes its count rather than every lane re-reading
    // all four tile ranges.
    if (localIndex == 0u) {
        sharedBucketBatchCount[bucketIndex] = batchCount;
    }
    workgroupBarrier();
    var maximumBatchCount = 0u;
    for (var candidateBucket = 0u;
         candidateBucket < RESOLVE_BUCKETS_PER_AXIS * RESOLVE_BUCKETS_PER_AXIS;
         candidateBucket += 1u) {
        maximumBatchCount = max(
            maximumBatchCount, sharedBucketBatchCount[candidateBucket]);
    }

    // Per-instance ray state, cached across candidates.
    //
    // Transforming the ray into instance space is invariant for every
    // candidate belonging to the same instance, but the loop below used to
    // redo it per candidate per pixel: a 240-byte instance load plus two mat4
    // transforms. Tile candidate lists are dominated by runs of the same
    // instance - a single-instance scene is entirely one run - so caching
    // collapses that to one computation per instance change. The arithmetic is
    // unchanged, so resolved surfaces are bit-identical.
    var cachedInstance = SPARSE_VOXEL_INVALID_INDEX;
    var cachedLocalOrigin = vec3<f32>(0.0);
    var cachedLocalDirection = vec3<f32>(0.0);
    var cachedQuadrant = 0u;

    var bestT = 3.402823466e+38;
    var bestInstance = SPARSE_VOXEL_INVALID_INDEX;
    var bestVoxel = SPARSE_VOXEL_INVALID_INDEX;
    var bestFace = 0u;
    var bestFaceScore = -1.0;
    var bestInside = 0u;
    for (var batch = 0u; batch < maximumBatchCount; batch += 1u) {
        let batchBegin = batch * RESOLVE_CANDIDATE_BATCH_SIZE;
        let loadOffset = batchBegin + localIndex;
        if (loadOffset < candidateCount) {
            let entry = range.begin + loadOffset;
            // The bucket path stores only the projected index and reconstructs
            // the rest; the record path is kept for the legacy emit/sort chain.
            // Identical values either way, so resolved surfaces are unchanged.
            var projectedIndex = 0u;
            var instanceIndex = 0u;
            var voxelIndex = 0u;
            var quadrant = 0u;
            var directCandidate = 0u;
            var minimumHitDistance = 0u;
            if (resolveControls.y != 0u) {
                projectedIndex = candidateIndices[entry];
                let projected = projectedVoxels[projectedIndex];
                instanceIndex = projected.instanceIndex;
                voxelIndex = projected.voxelIndex;
                quadrant = projected.quadrantBitset;
                minimumHitDistance = projected.reserved0;
                directCandidate = 1u;
            } else {
                let candidate = candidates[entry];
                projectedIndex = candidate.projectedVoxelIndex;
                instanceIndex = candidate.instanceIndex;
                voxelIndex = candidate.voxelIndex;
                quadrant = candidate.quadrant;
                minimumHitDistance = candidate.reserved0;
                directCandidate = candidate.reserved1;
            }
            let projected = projectedVoxels[projectedIndex];
            let centerLength = centersLengths[voxelIndex];
            let sharedIndex = sharedBase + localIndex;
            sharedCandidateMetadata[sharedIndex] = vec4<u32>(
                instanceIndex, voxelIndex, quadrant, directCandidate);
            sharedMinimumHitDistance[sharedIndex] = minimumHitDistance;
            sharedPackedBounds[sharedIndex] = projected.packedInclusivePixelBounds;
            sharedPaddedMinimum[sharedIndex] =
                vec4<f32>(paddedVoxelMinimum(centerLength), 0.0);
            sharedPaddedMaximum[sharedIndex] =
                vec4<f32>(paddedVoxelMaximum(centerLength), 0.0);
        }
        workgroupBarrier();

        if (insideViewport && batch < batchCount) {
            let candidatesInBatch = min(RESOLVE_CANDIDATE_BATCH_SIZE, candidateCount - batchBegin);
            // Establish a close hit as early as possible in the first batch.
            // Direct buckets arrive in atomic scatter order, so their first
            // item is otherwise arbitrary and the early distance rejection
            // below has little opportunity to help.
            //
            // First batch only. The guard used to be "no hit yet", which is
            // permanently true for a pixel that never hits anything, so sky and
            // background pixels paid a full extra scan of every batch for a
            // reordering that cannot help them. Reordering within a batch never
            // changes the result: hitPrecedes() is a total order and both early
            // rejections only skip candidates that cannot win it.
            var preferredCandidate = SPARSE_VOXEL_INVALID_INDEX;
            if (batch == 0u && bestVoxel == SPARSE_VOXEL_INVALID_INDEX) {
                var preferredDistance = 3.402823466e+38;
                for (var searchIndex = 0u; searchIndex < candidatesInBatch; searchIndex += 1u) {
                    let sharedSearchIndex = sharedBase + searchIndex;
                    if (!pixelCovered(pixel, sharedPackedBounds[sharedSearchIndex])) {
                        continue;
                    }
                    if (resolveControls.x != 0u &&
                        candidatePixelArea(sharedPackedBounds[sharedSearchIndex]) < resolveControls.x) {
                        continue;
                    }
                    let directCandidate = sharedCandidateMetadata[sharedSearchIndex].w != 0u;
                    let lowerBound = select(
                        0.0, bitcast<f32>(sharedMinimumHitDistance[sharedSearchIndex]),
                        directCandidate);
                    if (lowerBound < preferredDistance) {
                        preferredDistance = lowerBound;
                        preferredCandidate = searchIndex;
                    }
                }
            }
            for (var iteration = 0u; iteration < candidatesInBatch; iteration += 1u) {
                var batchIndex = iteration;
                if (preferredCandidate != SPARSE_VOXEL_INVALID_INDEX) {
                    if (iteration == 0u) {
                        batchIndex = preferredCandidate;
                    } else {
                        let originalIndex = iteration - 1u;
                        batchIndex = originalIndex + select(
                            0u, 1u, originalIndex >= preferredCandidate);
                    }
                }
                let sharedBatchIndex = sharedBase + batchIndex;
                if (!pixelCovered(pixel, sharedPackedBounds[sharedBatchIndex])) {
                    continue;
                }
                // Occluder-size filter for the pre-phase.
                //
                // The pre-phase exists only to produce a provisional HZB for
                // the final cull, and an HZB built from a subset of occluders
                // is conservative: it can only under-report occlusion, which
                // costs a few extra draws and never removes visible geometry.
                // Small voxels contribute almost nothing to occlusion but
                // dominate the candidate count, so skipping them cuts the
                // pre-phase sharply while leaving the large occluders that do
                // the actual culling work. The final phase passes zero here
                // and considers every candidate.
                if (resolveControls.x != 0u &&
                    candidatePixelArea(sharedPackedBounds[sharedBatchIndex]) < resolveControls.x) {
                    continue;
                }
                let metadata = sharedCandidateMetadata[sharedBatchIndex];
                let instanceIndex = metadata.x;
                let voxelIndex = metadata.y;
                if (bestVoxel != SPARSE_VOXEL_INVALID_INDEX &&
                    metadata.w != 0u) {
                    let lowerBoundKey =
                        (sharedMinimumHitDistance[sharedBatchIndex] + 128u) >> 8u;
                    let bestTKey =
                        (bitcast<u32>(max(bestT, 0.0)) + 128u) >> 8u;
                    if (lowerBoundKey > bestTKey) {
                        continue;
                    }
                }
                if (instanceIndex != cachedInstance) {
                    let instance = instances[instanceIndex];
                    cachedLocalOrigin = transformPoint(instance.worldToModel, worldOrigin);
                    cachedLocalDirection = transformVector(instance.worldToModel, worldDirection);
                    cachedQuadrant = rayQuadrant(cachedLocalDirection);
                    cachedInstance = instanceIndex;
                }
                let quadrantMatches = select(
                    cachedQuadrant == metadata.z,
                    (metadata.z & (1u << cachedQuadrant)) != 0u,
                    metadata.w != 0u);
                if (!quadrantMatches) {
                    continue;
                }
                let localOrigin = cachedLocalOrigin;
                let localDirection = cachedLocalDirection;
                let segment = voxelSegmentForPixel(
                    localOrigin,
                    localDirection,
                    sharedPaddedMinimum[sharedBatchIndex].xyz,
                    sharedPaddedMaximum[sharedBatchIndex].xyz,
                    bestT);
                if (segment.hit == 0u) {
                    continue;
                }
                let surfaceT = select(segment.tEnter, segment.tExit, segment.cameraInside != 0u);
                let face = select(segment.enterFace, segment.exitFace, segment.cameraInside != 0u);
                let faceScore = abs(localDirection[face & 3u]);
                if (hitPrecedes(surfaceT, instanceIndex, voxelIndex, faceScore, bestT, bestInstance, bestVoxel, bestFaceScore)) {
                    bestT = surfaceT;
                    bestInstance = instanceIndex;
                    bestVoxel = voxelIndex;
                    bestFace = face;
                    bestFaceScore = faceScore;
                    bestInside = segment.cameraInside;
                }
            }
        }
        workgroupBarrier();
    }

    if (insideViewport && bestVoxel != SPARSE_VOXEL_INVALID_INDEX) {
        let flags = SPARSE_VOXEL_SURFACE_HIT | select(0u, SPARSE_VOXEL_SURFACE_CAMERA_INSIDE, bestInside != 0u);
        resolvedSurfaces[pixelIndex] = ResolvedVoxelSurface(bestT, bestInstance, bestVoxel, bestFace, flags, 0u, 0u, 0u);
        coverage[pixelIndex] = 1u;
        linearDepth[pixelIndex] = bestT;
    }
}
