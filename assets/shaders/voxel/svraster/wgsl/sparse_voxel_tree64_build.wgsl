#include "sparse_voxel_tree64_common.wgsl"

struct SparseVoxelTree64BuildDispatch {
    side : u32,
    cellCount : u32,
    nodeSlot : u32,
    nodeSlotCapacity : u32,
    nodeCapacity : u32,
    contentGeneration : u32,
    reserved0 : u32,
    reserved1 : u32,
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

struct Tree64BuildMask {
    low : u32,
    high : u32,
}

@group(0) @binding(0) var<storage, read> occupiedCells : array<u32>;
@group(0) @binding(1) var<storage, read_write> outputNodes : array<SparseVoxelTree64Node>;
@group(0) @binding(2) var<storage, read_write> buildResult : SparseVoxelTree64BuildResult;
@group(0) @binding(3) var<uniform> dispatch : SparseVoxelTree64BuildDispatch;

const TREE64_BUILD_STATUS_INVALID = 0u;
const TREE64_BUILD_STATUS_SUCCESS = 1u;
const TREE64_BUILD_STATUS_OVERFLOW = 2u;
const TREE64_BUILD_OVERFLOW_PREFLIGHT = 1u;

fn tree64BuildSupportedSide(side : u32) -> bool {
    return side == 4u || side == 16u || side == 64u;
}

fn tree64BuildCellIndex(
    side : u32,
    cell : vec3<u32>) -> u32 {
    return cell.x + side * (cell.y + side * cell.z);
}

fn tree64BuildRegionOccupied(
    origin : vec3<u32>,
    size : u32) -> bool {
    for (var z = 0u; z < size; z += 1u) {
        for (var y = 0u; y < size; y += 1u) {
            for (var x = 0u; x < size; x += 1u) {
                if (occupiedCells[tree64BuildCellIndex(
                    dispatch.side,
                    origin + vec3<u32>(x, y, z))] != 0u) {
                    return true;
                }
            }
        }
    }
    return false;
}

fn tree64BuildSetMaskBit(
    mask : Tree64BuildMask,
    child : u32) -> Tree64BuildMask {
    var result = mask;
    if (child < 32u) {
        result.low |= 1u << child;
    } else {
        result.high |= 1u << (child - 32u);
    }
    return result;
}

fn tree64BuildChildOrigin(
    origin : vec3<u32>,
    child : u32,
    childSize : u32) -> vec3<u32> {
    return origin + vec3<u32>(
        child & 3u,
        (child >> 2u) & 3u,
        (child >> 4u) & 3u) * childSize;
}

fn tree64BuildRegionMask(
    origin : vec3<u32>,
    size : u32) -> Tree64BuildMask {
    var mask = Tree64BuildMask(0u, 0u);
    let childSize = size / 4u;
    for (var child = 0u; child < 64u; child += 1u) {
        if (tree64BuildRegionOccupied(
            tree64BuildChildOrigin(
                origin, child, childSize),
            childSize)) {
            mask = tree64BuildSetMaskBit(mask, child);
        }
    }
    return mask;
}

fn tree64BuildTerminalMask(
    origin : vec3<u32>) -> Tree64BuildMask {
    var mask = Tree64BuildMask(0u, 0u);
    for (var child = 0u; child < 64u; child += 1u) {
        let cell = tree64BuildChildOrigin(
            origin, child, 1u);
        if (occupiedCells[tree64BuildCellIndex(
            dispatch.side, cell)] != 0u) {
            mask = tree64BuildSetMaskBit(mask, child);
        }
    }
    return mask;
}

fn tree64BuildMaskCount(mask : Tree64BuildMask) -> u32 {
    return countOneBits(mask.low) + countOneBits(mask.high);
}

fn tree64BuildMaskContains(
    mask : Tree64BuildMask,
    child : u32) -> bool {
    let node = SparseVoxelTree64Node(
        0u, mask.low, mask.high);
    return tree64PopulationContains(node, child);
}

fn tree64BuildMaskCountBefore(
    mask : Tree64BuildMask,
    child : u32) -> u32 {
    let node = SparseVoxelTree64Node(
        0u, mask.low, mask.high);
    return tree64PopulationCountBefore(node, child);
}

fn tree64BuildRequiredNodeCount(
    rootMask : Tree64BuildMask) -> u32 {
    if (dispatch.side == 4u) {
        return 1u;
    }
    var required = 1u + tree64BuildMaskCount(rootMask);
    if (dispatch.side == 16u) {
        return required;
    }
    for (var rootChild = 0u;
         rootChild < 64u;
         rootChild += 1u) {
        if (!tree64BuildMaskContains(
            rootMask, rootChild)) {
            continue;
        }
        let middleOrigin = tree64BuildChildOrigin(
            vec3<u32>(0u), rootChild, 16u);
        let middleMask = tree64BuildRegionMask(
            middleOrigin, 16u);
        required += tree64BuildMaskCount(middleMask);
    }
    return required;
}

fn tree64BuildWriteTerminal(
    slotBase : u32,
    nodeIndex : u32,
    origin : vec3<u32>) {
    let mask = tree64BuildTerminalMask(origin);
    outputNodes[slotBase + nodeIndex] =
        SparseVoxelTree64Node(
            TREE64_TERMINAL_FLAG,
            mask.low,
            mask.high);
}

@compute @workgroup_size(1, 1, 1)
fn buildTree64Chunk(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    if (any(globalId != vec3<u32>(0u))) {
        return;
    }
    var output = SparseVoxelTree64BuildResult(
        TREE64_BUILD_STATUS_INVALID,
        0u,
        0u,
        dispatch.nodeSlot,
        dispatch.contentGeneration,
        0u,
        1u,
        0u);
    let expectedCellCount =
        dispatch.side * dispatch.side * dispatch.side;
    if (!tree64BuildSupportedSide(dispatch.side) ||
        dispatch.cellCount != expectedCellCount ||
        dispatch.nodeSlotCapacity == 0u ||
        dispatch.nodeSlot >
            dispatch.nodeCapacity /
                dispatch.nodeSlotCapacity ||
        dispatch.nodeSlot *
                dispatch.nodeSlotCapacity +
                dispatch.nodeSlotCapacity >
            dispatch.nodeCapacity) {
        buildResult = output;
        return;
    }

    var rootMask = Tree64BuildMask(0u, 0u);
    if (dispatch.side == 4u) {
        rootMask = tree64BuildTerminalMask(vec3<u32>(0u));
    } else {
        rootMask = tree64BuildRegionMask(
            vec3<u32>(0u), dispatch.side);
    }
    let required = tree64BuildRequiredNodeCount(rootMask);
    output.nodeCount = required;
    if (required > dispatch.nodeSlotCapacity) {
        output.status = TREE64_BUILD_STATUS_OVERFLOW;
        output.overflowStage =
            TREE64_BUILD_OVERFLOW_PREFLIGHT;
        buildResult = output;
        return;
    }

    let slotBase =
        dispatch.nodeSlot * dispatch.nodeSlotCapacity;
    if (dispatch.side == 4u) {
        outputNodes[slotBase] = SparseVoxelTree64Node(
            TREE64_TERMINAL_FLAG,
            rootMask.low,
            rootMask.high);
        output.status = TREE64_BUILD_STATUS_SUCCESS;
        buildResult = output;
        return;
    }

    let rootChildCount =
        tree64BuildMaskCount(rootMask);
    outputNodes[slotBase] = SparseVoxelTree64Node(
        select(0u, 2u, rootChildCount != 0u),
        rootMask.low,
        rootMask.high);
    if (dispatch.side == 16u) {
        for (var child = 0u; child < 64u; child += 1u) {
            if (!tree64BuildMaskContains(rootMask, child)) {
                continue;
            }
            tree64BuildWriteTerminal(
                slotBase,
                1u + tree64BuildMaskCountBefore(
                    rootMask, child),
                tree64BuildChildOrigin(
                    vec3<u32>(0u), child, 4u));
        }
        output.status = TREE64_BUILD_STATUS_SUCCESS;
        buildResult = output;
        return;
    }

    var nextNode = 1u + rootChildCount;
    for (var rootChild = 0u;
         rootChild < 64u;
         rootChild += 1u) {
        if (!tree64BuildMaskContains(
            rootMask, rootChild)) {
            continue;
        }
        let middleNode =
            1u + tree64BuildMaskCountBefore(
                rootMask, rootChild);
        let middleOrigin = tree64BuildChildOrigin(
            vec3<u32>(0u), rootChild, 16u);
        let middleMask = tree64BuildRegionMask(
            middleOrigin, 16u);
        let middleChildCount =
            tree64BuildMaskCount(middleMask);
        let middleChildBase = select(
            0u, nextNode, middleChildCount != 0u);
        outputNodes[slotBase + middleNode] =
            SparseVoxelTree64Node(
                middleChildBase << 1u,
                middleMask.low,
                middleMask.high);
        for (var middleChild = 0u;
             middleChild < 64u;
             middleChild += 1u) {
            if (!tree64BuildMaskContains(
                middleMask, middleChild)) {
                continue;
            }
            tree64BuildWriteTerminal(
                slotBase,
                middleChildBase +
                    tree64BuildMaskCountBefore(
                        middleMask, middleChild),
                tree64BuildChildOrigin(
                    middleOrigin, middleChild, 4u));
        }
        nextNode += middleChildCount;
    }
    output.status = TREE64_BUILD_STATUS_SUCCESS;
    buildResult = output;
}
