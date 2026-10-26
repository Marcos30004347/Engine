#include "sparse_voxel_tree64_common.wgsl"

struct SparseVoxelTree64BuildJob {
    worldChunk : vec3<i32>,
    nodeSlot : u32,
    contentGeneration : u32,
    flags : u32,
    reserved0 : u32,
    reserved1 : u32,
}

struct SparseVoxelTree64BuildSource {
    minimum : vec3<f32>,
    jobIndex : u32,
    maximum : vec3<f32>,
    reserved0 : u32,
}

struct SparseVoxelTree64BuildResult {
    status : u32,
    nodeCount : u32,
    rootNodeOffset : u32,
    nodeSlot : u32,
    contentGeneration : u32,
    overflowStage : u32,
    maximumStack : u32,
    reserved0 : u32,
}

struct SparseVoxelTree64BatchBuildDispatch {
    side : u32,
    cellCount : u32,
    chunkCount : u32,
    sourceCount : u32,
    nodeSlotCapacity : u32,
    nodeCapacity : u32,
    terminalCount : u32,
    middleCount : u32,
    cellCapacity : u32,
    terminalCapacity : u32,
    middleCapacity : u32,
    resultCapacity : u32,
    cellWordCount : u32,
    cellWordCapacity : u32,
    reserved2 : u32,
    reserved3 : u32,
}

struct Tree64BatchMask {
    low : u32,
    high : u32,
}

@group(0) @binding(0) var<storage, read> jobs : array<SparseVoxelTree64BuildJob>;
@group(0) @binding(1) var<storage, read> sources : array<SparseVoxelTree64BuildSource>;
@group(0) @binding(2) var<storage, read_write> cells : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read_write> terminalMasks : array<SparseVoxelTree64Node>;
@group(0) @binding(4) var<storage, read_write> middleMasks : array<SparseVoxelTree64Node>;
@group(0) @binding(5) var<storage, read_write> rootMasks : array<SparseVoxelTree64Node>;
@group(0) @binding(6) var<storage, read_write> outputNodes : array<SparseVoxelTree64Node>;
@group(0) @binding(7) var<storage, read_write> results : array<SparseVoxelTree64BuildResult>;
@group(0) @binding(8) var<uniform> dispatch : SparseVoxelTree64BatchBuildDispatch;

const TREE64_BATCH_JOB_VALID = 1u;
const TREE64_BATCH_STATUS_INVALID = 0u;
const TREE64_BATCH_STATUS_SUCCESS = 1u;
const TREE64_BATCH_STATUS_OVERFLOW = 2u;
const TREE64_BATCH_OVERFLOW_PREFLIGHT = 1u;

fn tree64BatchMask(node : SparseVoxelTree64Node)
    -> Tree64BatchMask {
    return Tree64BatchMask(
        node.populationMaskLo, node.populationMaskHi);
}

fn tree64BatchNode(mask : Tree64BatchMask)
    -> SparseVoxelTree64Node {
    return SparseVoxelTree64Node(0u, mask.low, mask.high);
}

fn tree64BatchMaskSet(
    mask : Tree64BatchMask,
    child : u32) -> Tree64BatchMask {
    var result = mask;
    if (child < 32u) {
        result.low |= 1u << child;
    } else {
        result.high |= 1u << (child - 32u);
    }
    return result;
}

fn tree64BatchMaskContains(
    mask : Tree64BatchMask,
    child : u32) -> bool {
    return tree64PopulationContains(
        tree64BatchNode(mask), child);
}

fn tree64BatchMaskCount(
    mask : Tree64BatchMask) -> u32 {
    return countOneBits(mask.low) +
        countOneBits(mask.high);
}

fn tree64BatchMaskCountBefore(
    mask : Tree64BatchMask,
    child : u32) -> u32 {
    return tree64PopulationCountBefore(
        tree64BatchNode(mask), child);
}

fn tree64BatchAxisIndex(
    index : u32,
    axis : u32) -> vec3<u32> {
    return vec3<u32>(
        index % axis,
        (index / axis) % axis,
        index / (axis * axis));
}

fn tree64BatchLocalCellIndex(cell : vec3<u32>) -> u32 {
    return cell.x + dispatch.side *
        (cell.y + dispatch.side * cell.z);
}

fn tree64BatchCellWordIndex(
    jobIndex : u32,
    localCellIndex : u32) -> u32 {
    return jobIndex * dispatch.cellWordCount +
        localCellIndex / 32u;
}

fn tree64BatchCellOccupied(
    jobIndex : u32,
    localCellIndex : u32) -> bool {
    let wordIndex = tree64BatchCellWordIndex(
        jobIndex, localCellIndex);
    return wordIndex < dispatch.cellWordCapacity &&
        (atomicLoad(&cells[wordIndex]) &
         (1u << (localCellIndex & 31u))) != 0u;
}

fn tree64BatchTerminalIndex(
    jobIndex : u32,
    terminal : vec3<u32>) -> u32 {
    let axis = dispatch.side / 4u;
    return jobIndex * dispatch.terminalCount +
        terminal.x + axis *
        (terminal.y + axis * terminal.z);
}

fn tree64BatchMiddleIndex(
    jobIndex : u32,
    middle : u32) -> u32 {
    return jobIndex * dispatch.middleCount + middle;
}

fn tree64BatchNonempty(node : SparseVoxelTree64Node) -> bool {
    return node.populationMaskLo != 0u ||
        node.populationMaskHi != 0u;
}

@compute @workgroup_size(64, 1, 1)
fn clearTree64Cells(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    let activeCount =
        dispatch.chunkCount * dispatch.cellWordCount;
    if (index >= activeCount ||
        index >= dispatch.cellWordCapacity) {
        return;
    }
    atomicStore(&cells[index], 0u);
}

@compute @workgroup_size(64, 1, 1)
fn splatTree64Sources(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let sourceIndex = globalId.x;
    if (sourceIndex >= dispatch.sourceCount) {
        return;
    }
    let source = sources[sourceIndex];
    if (source.jobIndex >= dispatch.chunkCount ||
        (jobs[source.jobIndex].flags &
         TREE64_BATCH_JOB_VALID) == 0u ||
        any(source.maximum <= source.minimum)) {
        return;
    }
    let side = i32(dispatch.side);
    let begin = clamp(
        vec3<i32>(floor(source.minimum)),
        vec3<i32>(0),
        vec3<i32>(side));
    let end = clamp(
        vec3<i32>(ceil(source.maximum)),
        vec3<i32>(0),
        vec3<i32>(side));
    if (any(begin >= end)) {
        return;
    }
    for (var z = begin.z; z < end.z; z += 1) {
        for (var y = begin.y; y < end.y; y += 1) {
            for (var x = begin.x; x < end.x; x += 1) {
                let localCellIndex = tree64BatchLocalCellIndex(
                    vec3<u32>(vec3<i32>(x, y, z)));
                let wordIndex = tree64BatchCellWordIndex(
                    source.jobIndex, localCellIndex);
                if (wordIndex < dispatch.cellWordCapacity) {
                    atomicOr(
                        &cells[wordIndex],
                        1u << (localCellIndex & 31u));
                }
            }
        }
    }
}

@compute @workgroup_size(64, 1, 1)
fn buildTree64TerminalMasks(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    let activeCount =
        dispatch.chunkCount * dispatch.terminalCount;
    if (index >= activeCount ||
        index >= dispatch.terminalCapacity) {
        return;
    }
    let jobIndex = index / dispatch.terminalCount;
    let localIndex = index % dispatch.terminalCount;
    let terminalCoord = tree64BatchAxisIndex(
        localIndex, dispatch.side / 4u);
    let cellOrigin = terminalCoord * 4u;
    var mask = Tree64BatchMask(0u, 0u);
    for (var child = 0u; child < 64u; child += 1u) {
        let offset = tree64BatchAxisIndex(child, 4u);
        let localCellIndex = tree64BatchLocalCellIndex(
            cellOrigin + offset);
        if (tree64BatchCellOccupied(
                jobIndex, localCellIndex)) {
            mask = tree64BatchMaskSet(mask, child);
        }
    }
    terminalMasks[index] = SparseVoxelTree64Node(
        TREE64_TERMINAL_FLAG, mask.low, mask.high);
}

@compute @workgroup_size(64, 1, 1)
fn buildTree64MiddleMasks(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    let activeCount =
        dispatch.chunkCount * dispatch.middleCount;
    if (dispatch.side != 64u ||
        index >= activeCount ||
        index >= dispatch.middleCapacity) {
        return;
    }
    let jobIndex = index / dispatch.middleCount;
    let middleIndex = index % dispatch.middleCount;
    let middleCoord = tree64BatchAxisIndex(middleIndex, 4u);
    var mask = Tree64BatchMask(0u, 0u);
    for (var child = 0u; child < 64u; child += 1u) {
        let terminalCoord =
            middleCoord * 4u +
            tree64BatchAxisIndex(child, 4u);
        let terminal = terminalMasks[
            tree64BatchTerminalIndex(
                jobIndex, terminalCoord)];
        if (tree64BatchNonempty(terminal)) {
            mask = tree64BatchMaskSet(mask, child);
        }
    }
    middleMasks[index] =
        SparseVoxelTree64Node(0u, mask.low, mask.high);
}

@compute @workgroup_size(64, 1, 1)
fn buildTree64RootMasks(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let jobIndex = globalId.x;
    if (jobIndex >= dispatch.chunkCount ||
        jobIndex >= dispatch.resultCapacity) {
        return;
    }
    if (dispatch.side == 4u) {
        rootMasks[jobIndex] =
            terminalMasks[
                jobIndex * dispatch.terminalCount];
        return;
    }
    var mask = Tree64BatchMask(0u, 0u);
    for (var child = 0u; child < 64u; child += 1u) {
        var occupied = false;
        if (dispatch.side == 16u) {
            occupied = tree64BatchNonempty(
                terminalMasks[
                    jobIndex * dispatch.terminalCount +
                    child]);
        } else {
            occupied = tree64BatchNonempty(
                middleMasks[
                    tree64BatchMiddleIndex(
                        jobIndex, child)]);
        }
        if (occupied) {
            mask = tree64BatchMaskSet(mask, child);
        }
    }
    rootMasks[jobIndex] =
        SparseVoxelTree64Node(0u, mask.low, mask.high);
}

fn tree64BatchInitialResult(
    job : SparseVoxelTree64BuildJob)
    -> SparseVoxelTree64BuildResult {
    return SparseVoxelTree64BuildResult(
        TREE64_BATCH_STATUS_INVALID,
        0u,
        0u,
        job.nodeSlot,
        job.contentGeneration,
        0u,
        1u,
        0u);
}

@compute @workgroup_size(64, 1, 1)
fn compactTree64Chunks(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let jobIndex = globalId.x;
    if (jobIndex >= dispatch.chunkCount ||
        jobIndex >= dispatch.resultCapacity) {
        return;
    }
    let job = jobs[jobIndex];
    var output = tree64BatchInitialResult(job);
    if ((job.flags & TREE64_BATCH_JOB_VALID) == 0u ||
        dispatch.nodeSlotCapacity == 0u ||
        job.nodeSlot >
            dispatch.nodeCapacity /
                dispatch.nodeSlotCapacity ||
        job.nodeSlot * dispatch.nodeSlotCapacity +
                dispatch.nodeSlotCapacity >
            dispatch.nodeCapacity) {
        results[jobIndex] = output;
        return;
    }
    let root = rootMasks[jobIndex];
    let rootMask = tree64BatchMask(root);
    var required = 1u;
    if (dispatch.side != 4u) {
        required += tree64BatchMaskCount(rootMask);
    }
    if (dispatch.side == 64u) {
        for (var child = 0u; child < 64u; child += 1u) {
            if (tree64BatchMaskContains(rootMask, child)) {
                required += tree64BatchMaskCount(
                    tree64BatchMask(
                        middleMasks[
                            tree64BatchMiddleIndex(
                                jobIndex, child)]));
            }
        }
    }
    output.nodeCount = required;
    if (required > dispatch.nodeSlotCapacity) {
        output.status = TREE64_BATCH_STATUS_OVERFLOW;
        output.overflowStage =
            TREE64_BATCH_OVERFLOW_PREFLIGHT;
        results[jobIndex] = output;
        return;
    }

    let slotBase =
        job.nodeSlot * dispatch.nodeSlotCapacity;
    if (dispatch.side == 4u) {
        outputNodes[slotBase] = root;
        output.status = TREE64_BATCH_STATUS_SUCCESS;
        results[jobIndex] = output;
        return;
    }

    let rootChildCount =
        tree64BatchMaskCount(rootMask);
    outputNodes[slotBase] = SparseVoxelTree64Node(
        select(0u, 2u, rootChildCount != 0u),
        rootMask.low,
        rootMask.high);
    if (dispatch.side == 16u) {
        for (var child = 0u; child < 64u; child += 1u) {
            if (!tree64BatchMaskContains(rootMask, child)) {
                continue;
            }
            outputNodes[
                slotBase + 1u +
                tree64BatchMaskCountBefore(
                    rootMask, child)] =
                terminalMasks[
                    jobIndex * dispatch.terminalCount +
                    child];
        }
        output.status = TREE64_BATCH_STATUS_SUCCESS;
        results[jobIndex] = output;
        return;
    }

    var nextNode = 1u + rootChildCount;
    for (var rootChild = 0u;
         rootChild < 64u;
         rootChild += 1u) {
        if (!tree64BatchMaskContains(
            rootMask, rootChild)) {
            continue;
        }
        let middle =
            middleMasks[
                tree64BatchMiddleIndex(
                    jobIndex, rootChild)];
        let middleMask = tree64BatchMask(middle);
        let middleChildCount =
            tree64BatchMaskCount(middleMask);
        let middleChildBase = select(
            0u, nextNode, middleChildCount != 0u);
        let middleNode =
            1u + tree64BatchMaskCountBefore(
                rootMask, rootChild);
        outputNodes[slotBase + middleNode] =
            SparseVoxelTree64Node(
                middleChildBase << 1u,
                middleMask.low,
                middleMask.high);
        let middleCoord =
            tree64BatchAxisIndex(rootChild, 4u);
        for (var middleChild = 0u;
             middleChild < 64u;
             middleChild += 1u) {
            if (!tree64BatchMaskContains(
                middleMask, middleChild)) {
                continue;
            }
            let terminalCoord =
                middleCoord * 4u +
                tree64BatchAxisIndex(
                    middleChild, 4u);
            outputNodes[
                slotBase + middleChildBase +
                tree64BatchMaskCountBefore(
                    middleMask, middleChild)] =
                terminalMasks[
                    tree64BatchTerminalIndex(
                        jobIndex, terminalCoord)];
        }
        nextNode += middleChildCount;
    }
    output.status = TREE64_BATCH_STATUS_SUCCESS;
    results[jobIndex] = output;
}
