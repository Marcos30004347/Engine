struct SparseVoxelOccupancyVolume {
    logicalResolution : vec3<u32>,
    wordsX : u32,
    wordResolution : vec3<u32>,
    flags : u32,
    cameraCell : vec3<i32>,
    contentGeneration : u32,
    originCell : vec3<i32>,
    sceneRevision : u32,
    scrollDelta : vec3<i32>,
    dirtyWordCount : u32,
    wrapOffset : vec3<u32>,
    valid : u32,
    cameraInnerCell : vec3<f32>,
    fullRebuild : u32,
    cellSize : vec3<f32>,
    reserved0 : u32,
    worldExtent : vec3<f32>,
    overflowStage : u32,
}

struct SparseVoxelOccupancyDirtyWord {
    wordIndex : u32,
    bitMask : u32,
}

struct SparseVoxelOccupancyDispatch {
    reserved0 : u32,
    sourceAABBCount : u32,
    reserved1 : u32,
    reserved2 : u32,
}

struct SparseVoxelOccupancyAABB {
    minimum : vec4<f32>,
    maximum : vec4<f32>,
}

struct SparseVoxelOccupancyMipLevel {
    logicalResolution : vec3<u32>,
    wordsX : u32,
    wordResolution : vec3<u32>,
    level : u32,
    originCell : vec3<i32>,
    scale : u32,
    wrapOffset : vec3<u32>,
    wordOffset : u32,
    activeResolution : vec3<u32>,
    wordCount : u32,
}

struct SparseVoxelOccupancyMipBuild {
    baseState : SparseVoxelOccupancyVolume,
    childLevel : SparseVoxelOccupancyMipLevel,
    parentLevel : SparseVoxelOccupancyMipLevel,
    dirtyWordCount : u32,
    reserved0 : u32,
    reserved1 : u32,
    reserved2 : u32,
}

struct SparseVoxelOccupancyPyramid {
    contentGeneration : u32,
    levelCount : u32,
    valid : u32,
    baseDirtyWordCount : u32,
    mipDirtyWordCount : u32,
    mipWordCount : u32,
    fullRebuild : u32,
    reserved0 : u32,
}

struct SparseVoxelOccupancyAddress {
    physicalCell : vec3<u32>,
    wordIndex : u32,
    bitMask : u32,
}

fn sparseVoxelOccupancyPositiveModulo(value : i32, modulus : i32) -> u32 {
    let remainder = value % modulus;
    return u32(select(remainder, remainder + modulus, remainder < 0));
}

fn sparseVoxelOccupancyPhysicalCell(
    logical : vec3<u32>,
    state : SparseVoxelOccupancyVolume) -> vec3<u32> {
    return (logical + state.wrapOffset) % state.logicalResolution;
}

fn sparseVoxelOccupancyAddress(
    logical : vec3<u32>,
    state : SparseVoxelOccupancyVolume) -> SparseVoxelOccupancyAddress {
    let physical = sparseVoxelOccupancyPhysicalCell(logical, state);
    let wordCoord = vec3<u32>(physical.x >> 5u, physical.y, physical.z);
    let wordIndex =
        wordCoord.x +
        state.wordResolution.x *
            (wordCoord.y + state.wordResolution.y * wordCoord.z);
    return SparseVoxelOccupancyAddress(
        physical,
        wordIndex,
        1u << (physical.x & 31u));
}

fn sparseVoxelOccupancyLogicalCellFromPhysical(
    physical : vec3<u32>,
    state : SparseVoxelOccupancyVolume) -> vec3<u32> {
    return vec3<u32>(
        sparseVoxelOccupancyPositiveModulo(
            i32(physical.x) - i32(state.wrapOffset.x),
            i32(state.logicalResolution.x)),
        sparseVoxelOccupancyPositiveModulo(
            i32(physical.y) - i32(state.wrapOffset.y),
            i32(state.logicalResolution.y)),
        sparseVoxelOccupancyPositiveModulo(
            i32(physical.z) - i32(state.wrapOffset.z),
            i32(state.logicalResolution.z)));
}

fn sparseVoxelOccupancyNewlyExposed1D(
    coordinate : u32,
    delta : i32,
    extent : u32) -> bool {
    if (delta > 0) {
        return coordinate >= extent - u32(delta);
    }
    if (delta < 0) {
        return coordinate < u32(-delta);
    }
    return false;
}

fn sparseVoxelOccupancyDirtyLogicalCell(
    logical : vec3<u32>,
    state : SparseVoxelOccupancyVolume) -> bool {
    return state.fullRebuild != 0u ||
        sparseVoxelOccupancyNewlyExposed1D(
            logical.x, state.scrollDelta.x, state.logicalResolution.x) ||
        sparseVoxelOccupancyNewlyExposed1D(
            logical.y, state.scrollDelta.y, state.logicalResolution.y) ||
        sparseVoxelOccupancyNewlyExposed1D(
            logical.z, state.scrollDelta.z, state.logicalResolution.z);
}

fn sparseVoxelOccupancyWordCoord(
    wordIndex : u32,
    state : SparseVoxelOccupancyVolume) -> vec3<u32> {
    let wordX = wordIndex % state.wordResolution.x;
    let yz = wordIndex / state.wordResolution.x;
    return vec3<u32>(
        wordX,
        yz % state.wordResolution.y,
        yz / state.wordResolution.y);
}

fn sparseVoxelOccupancyMipPhysicalCell(
    logical : vec3<u32>,
    state : SparseVoxelOccupancyMipLevel) -> vec3<u32> {
    return (logical + state.wrapOffset) % state.logicalResolution;
}

fn sparseVoxelOccupancyMipAddress(
    logical : vec3<u32>,
    state : SparseVoxelOccupancyMipLevel) -> SparseVoxelOccupancyAddress {
    let physical = sparseVoxelOccupancyMipPhysicalCell(logical, state);
    let wordCoord = vec3<u32>(physical.x >> 5u, physical.y, physical.z);
    let wordIndex =
        wordCoord.x +
        state.wordResolution.x *
            (wordCoord.y + state.wordResolution.y * wordCoord.z);
    return SparseVoxelOccupancyAddress(
        physical,
        wordIndex,
        1u << (physical.x & 31u));
}

fn sparseVoxelOccupancyMipLogicalCellFromPhysical(
    physical : vec3<u32>,
    state : SparseVoxelOccupancyMipLevel) -> vec3<u32> {
    return vec3<u32>(
        sparseVoxelOccupancyPositiveModulo(
            i32(physical.x) - i32(state.wrapOffset.x),
            i32(state.logicalResolution.x)),
        sparseVoxelOccupancyPositiveModulo(
            i32(physical.y) - i32(state.wrapOffset.y),
            i32(state.logicalResolution.y)),
        sparseVoxelOccupancyPositiveModulo(
            i32(physical.z) - i32(state.wrapOffset.z),
            i32(state.logicalResolution.z)));
}

fn sparseVoxelOccupancyMipWordCoord(
    wordIndex : u32,
    state : SparseVoxelOccupancyMipLevel) -> vec3<u32> {
    let wordX = wordIndex % state.wordResolution.x;
    let yz = wordIndex / state.wordResolution.x;
    return vec3<u32>(
        wordX,
        yz % state.wordResolution.y,
        yz / state.wordResolution.y);
}

fn sparseVoxelOccupancyMipContainsGlobalCell(
    globalCell : vec3<i32>,
    state : SparseVoxelOccupancyMipLevel) -> bool {
    let logical = globalCell - state.originCell;
    return all(logical >= vec3<i32>(0)) &&
        all(logical < vec3<i32>(state.activeResolution));
}

fn sparseVoxelOccupancyAtWorldPosition(
    worldPosition : vec3<f32>,
    state : SparseVoxelOccupancyVolume,
    packedWord : u32) -> bool {
    let globalCell = vec3<i32>(floor(worldPosition / state.cellSize));
    let logical = globalCell - state.originCell;
    if (any(logical < vec3<i32>(0)) ||
        any(logical >= vec3<i32>(state.logicalResolution))) {
        return false;
    }
    let address = sparseVoxelOccupancyAddress(vec3<u32>(logical), state);
    return (packedWord & address.bitMask) != 0u;
}
