struct ChunkKey {
    worldRevisionLo : u32,
    worldRevisionHi : u32,
    chunkX : i32,
    chunkY : i32,
    chunkZ : i32,
    packedLodRevision : u32,
    terrainSlot : u32,
    expectedGeneration : u32,
}

struct CoverageBox {
    minimumX : i32,
    minimumY : i32,
    minimumZ : i32,
    maximumX : i32,
    maximumY : i32,
    maximumZ : i32,
}

struct PayloadResult {
    status : u32,
    validationErrors : u32,
    flags : u32,
    rasterNodeCount : u32,
    rasterMaterialCount : u32,
    rasterChildCount : u32,
    rasterRootCount : u32,
    rasterOccupiedVoxelCount : u32,
    terrainGeneration : u32,
    rasterGeneratorRevision : u32,
    treeNodeCount : u32,
    treeVisitedNodeCount : u32,
    treeOccupiedVoxelCount : u32,
    treeGeneratorRevision : u32,
    geometryRevisionLow : u32,
    geometryRevisionHigh : u32,
}

struct PublicationMember {
    key : ChunkKey,
    coverage : CoverageBox,
    payloadResultIndex : u32,
    ownerFlags : u32,
    leaseIdLow : u32,
    leaseIdHigh : u32,
    leaseOffsetLow : u32,
    leaseOffsetHigh : u32,
    leaseBytesLow : u32,
    leaseBytesHigh : u32,
    reserved0 : u32,
    reserved1 : u32,
}

struct PublishedOwner {
    key : ChunkKey,
    coverage : CoverageBox,
    ownerFlags : u32,
    payloadResultIndex : u32,
    leaseIdLow : u32,
    leaseIdHigh : u32,
    leaseOffsetLow : u32,
    leaseOffsetHigh : u32,
    leaseBytesLow : u32,
    leaseBytesHigh : u32,
    payloadRevisionLow : u32,
    payloadRevisionHigh : u32,
}

struct OwnershipGroup {
    epoch : atomic<u32>,
    terrainGeneration : u32,
    payloadRevisionLow : u32,
    payloadRevisionHigh : u32,
    ownerOffset : u32,
    ownerCount : u32,
    flags : u32,
    reserved0 : u32,
    coverage : CoverageBox,
    reserved1 : u32,
    reserved2 : u32,
}

struct PublicationRequest {
    groupIndex : u32,
    expectedEpoch : u32,
    expectedTerrainGeneration : u32,
    expectedPayloadRevisionLow : u32,
    expectedPayloadRevisionHigh : u32,
    replacementTerrainGeneration : u32,
    replacementPayloadRevisionLow : u32,
    replacementPayloadRevisionHigh : u32,
    memberOffset : u32,
    memberCount : u32,
    ownerOffset : u32,
    ownerCapacity : u32,
    expectedOwnerOffset : u32,
    expectedOwnerCount : u32,
    flags : u32,
    reserved : u32,
}

struct PublicationResult {
    status : u32,
    rejectionReason : u32,
    groupIndex : u32,
    committedEpoch : u32,
    ownerCount : u32,
    terrainGeneration : u32,
    payloadRevisionLow : u32,
    payloadRevisionHigh : u32,
}

struct PublicationBatch {
    requestCount : u32,
    memberCount : u32,
    payloadResultCount : u32,
    groupCount : u32,
    ownerCount : u32,
    reserved0 : u32,
    reserved1 : u32,
    reserved2 : u32,
}

struct Wide {
    low : u32,
    high : u32,
}

struct CheckedWide {
    value : Wide,
    valid : bool,
}

const STATUS_APPROVED : u32 = 1u;
const STATUS_COMMITTED : u32 = 2u;
const STATUS_REJECTED : u32 = 3u;
const REJECT_NONE : u32 = 0u;
const REJECT_INVALID_REQUEST : u32 = 1u;
const REJECT_INVALID_PAYLOAD : u32 = 2u;
const REJECT_GENERATION : u32 = 3u;
const REJECT_REVISION : u32 = 4u;
const REJECT_OCCUPANCY : u32 = 5u;
const REJECT_COVERAGE : u32 = 6u;
const REJECT_STALE : u32 = 7u;
const PAYLOAD_REQUIRED_FLAGS : u32 = 3u;

@group(0) @binding(0) var<storage, read> batchBuffer : array<PublicationBatch>;
@group(0) @binding(1) var<storage, read> requests : array<PublicationRequest>;
@group(0) @binding(2) var<storage, read> members : array<PublicationMember>;
@group(0) @binding(3) var<storage, read> payloads : array<PayloadResult>;
@group(0) @binding(4) var<storage, read_write> groups : array<OwnershipGroup>;
@group(0) @binding(5) var<storage, read_write> owners : array<PublishedOwner>;
@group(0) @binding(6) var<storage, read_write> results : array<PublicationResult>;

fn rangeFits(offset : u32, count : u32, capacity : u32) -> bool {
    return offset <= capacity && count <= capacity - offset;
}

fn boxValid(box : CoverageBox) -> bool {
    return box.minimumX < box.maximumX && box.minimumY < box.maximumY && box.minimumZ < box.maximumZ;
}

fn boxContained(part : CoverageBox, whole : CoverageBox) -> bool {
    return part.minimumX >= whole.minimumX && part.minimumY >= whole.minimumY && part.minimumZ >= whole.minimumZ &&
        part.maximumX <= whole.maximumX && part.maximumY <= whole.maximumY && part.maximumZ <= whole.maximumZ;
}

fn boxesOverlap(a : CoverageBox, b : CoverageBox) -> bool {
    return a.maximumX > b.minimumX && b.maximumX > a.minimumX &&
        a.maximumY > b.minimumY && b.maximumY > a.minimumY &&
        a.maximumZ > b.minimumZ && b.maximumZ > a.minimumZ;
}

fn multiply32(a : u32, b : u32) -> Wide {
    let a0 = a & 0xffffu;
    let a1 = a >> 16u;
    let b0 = b & 0xffffu;
    let b1 = b >> 16u;
    let first = a0 * b0;
    let middle0 = a1 * b0 + (first >> 16u);
    let middle1 = (middle0 & 0xffffu) + a0 * b1;
    return Wide((middle1 << 16u) | (first & 0xffffu), a1 * b1 + (middle0 >> 16u) + (middle1 >> 16u));
}

fn multiplyWide(value : Wide, factor : u32) -> CheckedWide {
    let lowProduct = multiply32(value.low, factor);
    let highProduct = multiply32(value.high, factor);
    let high = lowProduct.high + highProduct.low;
    let overflow = highProduct.high != 0u || high < lowProduct.high;
    return CheckedWide(Wide(lowProduct.low, high), !overflow);
}

fn addWide(a : Wide, b : Wide) -> CheckedWide {
    let low = a.low + b.low;
    let carry = select(0u, 1u, low < a.low);
    let high0 = a.high + b.high;
    let high = high0 + carry;
    return CheckedWide(Wide(low, high), high0 >= a.high && high >= high0);
}

fn boxVolume(box : CoverageBox) -> CheckedWide {
    if (!boxValid(box)) { return CheckedWide(Wide(0u, 0u), false); }
    let x = bitcast<u32>(box.maximumX) - bitcast<u32>(box.minimumX);
    let y = bitcast<u32>(box.maximumY) - bitcast<u32>(box.minimumY);
    let z = bitcast<u32>(box.maximumZ) - bitcast<u32>(box.minimumZ);
    let xy = multiplyWide(Wide(x, 0u), y);
    if (!xy.valid) { return xy; }
    return multiplyWide(xy.value, z);
}

fn payloadRejection(payload : PayloadResult) -> u32 {
    if (payload.rasterOccupiedVoxelCount != payload.treeOccupiedVoxelCount) { return REJECT_OCCUPANCY; }
    if (payload.status != STATUS_APPROVED || payload.validationErrors != 0u ||
        (payload.flags & PAYLOAD_REQUIRED_FLAGS) != PAYLOAD_REQUIRED_FLAGS ||
        payload.rasterNodeCount == 0u || payload.rasterRootCount == 0u || payload.treeNodeCount == 0u ||
        payload.treeVisitedNodeCount != payload.treeNodeCount || payload.terrainGeneration == 0u) {
        return REJECT_INVALID_PAYLOAD;
    }
    return REJECT_NONE;
}

fn rejection(index : u32, groupIndex : u32, reason : u32) {
    results[index] = PublicationResult(STATUS_REJECTED, reason, groupIndex, 0u, 0u, 0u, 0u, 0u);
}

@compute @workgroup_size(64, 1, 1)
fn validateTerrainGPUV2Publication(@builtin(global_invocation_id) gid : vec3<u32>) {
    let index = gid.x;
    let batch = batchBuffer[0];
    if (index >= batch.requestCount || index >= arrayLength(&requests) || index >= arrayLength(&results)) { return; }
    let request = requests[index];
    if (request.groupIndex >= batch.groupCount || request.groupIndex >= arrayLength(&groups) ||
        (request.expectedEpoch & 1u) != 0u || request.memberCount == 0u || request.memberCount > 8u ||
        !rangeFits(request.memberOffset, request.memberCount, min(batch.memberCount, arrayLength(&members))) ||
        request.ownerCapacity < request.memberCount ||
        !rangeFits(request.ownerOffset, request.ownerCapacity, min(batch.ownerCount, arrayLength(&owners))) ||
        !rangeFits(request.expectedOwnerOffset, request.expectedOwnerCount, min(batch.ownerCount, arrayLength(&owners))) ||
        request.replacementTerrainGeneration == 0u || (request.replacementPayloadRevisionLow | request.replacementPayloadRevisionHigh) == 0u) {
        rejection(index, request.groupIndex, REJECT_INVALID_REQUEST);
        return;
    }
    for (var prior = 0u; prior < index; prior++) {
        let other = requests[prior];
        if (!rangeFits(other.ownerOffset, other.ownerCapacity, min(batch.ownerCount, arrayLength(&owners)))) {
            rejection(index, request.groupIndex, REJECT_INVALID_REQUEST);
            return;
        }
        let overlap = request.ownerOffset < other.ownerOffset + other.ownerCapacity && other.ownerOffset < request.ownerOffset + request.ownerCapacity;
        if (request.groupIndex == other.groupIndex || overlap) {
            rejection(index, request.groupIndex, REJECT_INVALID_REQUEST);
            return;
        }
    }
    let groupCoverage = groups[request.groupIndex].coverage;
    if (!boxValid(groupCoverage)) {
        rejection(index, request.groupIndex, REJECT_COVERAGE);
        return;
    }
    if (!rangeFits(groups[request.groupIndex].ownerOffset, groups[request.groupIndex].ownerCount, min(batch.ownerCount, arrayLength(&owners)))) {
        rejection(index, request.groupIndex, REJECT_INVALID_REQUEST);
        return;
    }
    if (atomicLoad(&groups[request.groupIndex].epoch) != request.expectedEpoch ||
        groups[request.groupIndex].terrainGeneration != request.expectedTerrainGeneration || groups[request.groupIndex].payloadRevisionLow != request.expectedPayloadRevisionLow ||
        groups[request.groupIndex].payloadRevisionHigh != request.expectedPayloadRevisionHigh || groups[request.groupIndex].ownerOffset != request.expectedOwnerOffset ||
        groups[request.groupIndex].ownerCount != request.expectedOwnerCount) {
        rejection(index, request.groupIndex, REJECT_STALE);
        return;
    }
    let wholeVolume = boxVolume(groupCoverage);
    var volume = Wide(0u, 0u);
    for (var offset = 0u; offset < request.memberCount; offset++) {
        let member = members[request.memberOffset + offset];
        if (member.payloadResultIndex >= batch.payloadResultCount || member.payloadResultIndex >= arrayLength(&payloads) ||
            (member.key.worldRevisionLo | member.key.worldRevisionHi) == 0u || member.key.expectedGeneration == 0u ||
            (member.leaseIdLow | member.leaseIdHigh) == 0u || (member.leaseBytesLow | member.leaseBytesHigh) == 0u ||
            !boxContained(member.coverage, groupCoverage)) {
            rejection(index, request.groupIndex, REJECT_INVALID_REQUEST);
            return;
        }
        for (var prior = 0u; prior < offset; prior++) {
            if (boxesOverlap(member.coverage, members[request.memberOffset + prior].coverage)) {
                rejection(index, request.groupIndex, REJECT_COVERAGE);
                return;
            }
        }
        let partVolume = boxVolume(member.coverage);
        let sum = addWide(volume, partVolume.value);
        if (!partVolume.valid || !sum.valid) {
            rejection(index, request.groupIndex, REJECT_COVERAGE);
            return;
        }
        volume = sum.value;
        let payload = payloads[member.payloadResultIndex];
        let payloadError = payloadRejection(payload);
        if (payloadError != REJECT_NONE) {
            rejection(index, request.groupIndex, payloadError);
            return;
        }
        if (payload.terrainGeneration != request.replacementTerrainGeneration || member.key.expectedGeneration != request.replacementTerrainGeneration) {
            rejection(index, request.groupIndex, REJECT_GENERATION);
            return;
        }
        if (payload.geometryRevisionLow != request.replacementPayloadRevisionLow || payload.geometryRevisionHigh != request.replacementPayloadRevisionHigh) {
            rejection(index, request.groupIndex, REJECT_REVISION);
            return;
        }
    }
    if (!wholeVolume.valid || volume.low != wholeVolume.value.low || volume.high != wholeVolume.value.high) {
        rejection(index, request.groupIndex, REJECT_COVERAGE);
        return;
    }
    results[index] = PublicationResult(STATUS_APPROVED, REJECT_NONE, request.groupIndex, 0u, request.memberCount,
        request.replacementTerrainGeneration, request.replacementPayloadRevisionLow, request.replacementPayloadRevisionHigh);
}

fn ownerFrom(member : PublicationMember, revisionLow : u32, revisionHigh : u32) -> PublishedOwner {
    return PublishedOwner(member.key, member.coverage, member.ownerFlags, member.payloadResultIndex,
        member.leaseIdLow, member.leaseIdHigh, member.leaseOffsetLow, member.leaseOffsetHigh,
        member.leaseBytesLow, member.leaseBytesHigh, revisionLow, revisionHigh);
}

@compute @workgroup_size(64, 1, 1)
fn commitTerrainGPUV2Publication(@builtin(global_invocation_id) gid : vec3<u32>) {
    let index = gid.x;
    let batch = batchBuffer[0];
    if (index >= batch.requestCount || index >= arrayLength(&requests) || index >= arrayLength(&results) || results[index].status != STATUS_APPROVED) { return; }
    let request = requests[index];
    loop {
        let claim = atomicCompareExchangeWeak(&groups[request.groupIndex].epoch, request.expectedEpoch, request.expectedEpoch + 1u);
        if (claim.exchanged) { break; }
        if (claim.old_value != request.expectedEpoch) {
            rejection(index, request.groupIndex, REJECT_STALE);
            return;
        }
    }
    if (groups[request.groupIndex].terrainGeneration != request.expectedTerrainGeneration || groups[request.groupIndex].payloadRevisionLow != request.expectedPayloadRevisionLow ||
        groups[request.groupIndex].payloadRevisionHigh != request.expectedPayloadRevisionHigh || groups[request.groupIndex].ownerOffset != request.expectedOwnerOffset ||
        groups[request.groupIndex].ownerCount != request.expectedOwnerCount) {
        atomicStore(&groups[request.groupIndex].epoch, request.expectedEpoch);
        rejection(index, request.groupIndex, REJECT_STALE);
        return;
    }
    for (var offset = 0u; offset < request.memberCount; offset++) {
        let member = members[request.memberOffset + offset];
        owners[request.ownerOffset + offset] = ownerFrom(member, request.replacementPayloadRevisionLow, request.replacementPayloadRevisionHigh);
    }
    groups[request.groupIndex].terrainGeneration = request.replacementTerrainGeneration;
    groups[request.groupIndex].payloadRevisionLow = request.replacementPayloadRevisionLow;
    groups[request.groupIndex].payloadRevisionHigh = request.replacementPayloadRevisionHigh;
    groups[request.groupIndex].ownerOffset = request.ownerOffset;
    groups[request.groupIndex].ownerCount = request.memberCount;
    groups[request.groupIndex].flags = request.flags;
    atomicStore(&groups[request.groupIndex].epoch, request.expectedEpoch + 2u);
    results[index] = PublicationResult(STATUS_COMMITTED, REJECT_NONE, request.groupIndex, request.expectedEpoch + 2u,
        request.memberCount, request.replacementTerrainGeneration, request.replacementPayloadRevisionLow, request.replacementPayloadRevisionHigh);
}
