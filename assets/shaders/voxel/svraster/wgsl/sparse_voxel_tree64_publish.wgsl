#include "sparse_voxel_tree64_common.wgsl"

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

struct SparseVoxelTree64PublishRecord {
    worldChunk : vec3<i32>,
    directoryIndex : u32,
    nodeSlot : u32,
    contentGeneration : u32,
    resultIndex : u32,
    reserved0 : u32,
}

struct SparseVoxelTree64PublishDispatch {
    recordCount : u32,
    recordCapacity : u32,
    resultCapacity : u32,
    directoryCapacity : u32,
    nodeSlotCapacity : u32,
    nodeCapacity : u32,
    reserved0 : u32,
    reserved1 : u32,
}

@group(0) @binding(0)
var<storage, read> buildResults :
    array<SparseVoxelTree64BuildResult>;
@group(0) @binding(1)
var<storage, read> nodes :
    array<SparseVoxelTree64Node>;
@group(0) @binding(2)
var<storage, read> records :
    array<SparseVoxelTree64PublishRecord>;
@group(0) @binding(3)
var<storage, read> targetState :
    SparseVoxelTree64State;
@group(0) @binding(4)
var<storage, read_write> directory :
    array<SparseVoxelTree64Chunk>;
@group(0) @binding(5)
var<storage, read_write> committedState :
    SparseVoxelTree64State;
@group(0) @binding(6)
var<uniform> dispatch :
    SparseVoxelTree64PublishDispatch;

const TREE64_PUBLISH_BUILD_SUCCESS = 1u;
const TREE64_PUBLISH_CHUNK_VALID = 1u;
const TREE64_PUBLISH_CHUNK_UNKNOWN = 2u;
const TREE64_PUBLISH_CHUNK_OCCUPIED = 4u;
const TREE64_PUBLISH_RECORD_EMPTY = 1u;

fn tree64PublishUnknown(
    record : SparseVoxelTree64PublishRecord)
    -> SparseVoxelTree64Chunk {
    return SparseVoxelTree64Chunk(
        record.worldChunk,
        0u,
        0u,
        0u,
        record.contentGeneration,
        TREE64_PUBLISH_CHUNK_VALID |
            TREE64_PUBLISH_CHUNK_UNKNOWN);
}

@compute @workgroup_size(64, 1, 1)
fn publishTree64Directory(
    @builtin(global_invocation_id)
    globalId : vec3<u32>) {
    let index = globalId.x;
    if (index == 0u) {
        committedState = targetState;
    }
    if (index >= dispatch.recordCount ||
        index >= dispatch.recordCapacity) {
        return;
    }

    let record = records[index];
    if (record.directoryIndex >=
            dispatch.directoryCapacity) {
        return;
    }
    var descriptor = tree64PublishUnknown(record);
    if ((record.reserved0 &
            TREE64_PUBLISH_RECORD_EMPTY) != 0u) {
        descriptor = SparseVoxelTree64Chunk(
            record.worldChunk,
            record.nodeSlot,
            0u,
            0u,
            record.contentGeneration,
            TREE64_PUBLISH_CHUNK_VALID);
    } else if (record.resultIndex < dispatch.resultCapacity) {
        let result = buildResults[record.resultIndex];
        let slotBase =
            record.nodeSlot * dispatch.nodeSlotCapacity;
        let validSlot =
            dispatch.nodeSlotCapacity != 0u &&
            record.nodeSlot <=
                dispatch.nodeCapacity /
                    dispatch.nodeSlotCapacity &&
            slotBase + dispatch.nodeSlotCapacity <=
                dispatch.nodeCapacity;
        let validResult =
            result.status ==
                TREE64_PUBLISH_BUILD_SUCCESS &&
            result.nodeSlot == record.nodeSlot &&
            result.contentGeneration ==
                record.contentGeneration &&
            result.nodeCount != 0u &&
            result.nodeCount <=
                dispatch.nodeSlotCapacity &&
            result.rootNodeOffset <
                result.nodeCount &&
            validSlot;
        if (validResult) {
            let root = nodes[
                slotBase + result.rootNodeOffset];
            let occupied =
                root.populationMaskLo != 0u ||
                root.populationMaskHi != 0u;
            descriptor = SparseVoxelTree64Chunk(
                record.worldChunk,
                record.nodeSlot,
                result.rootNodeOffset,
                result.nodeCount,
                record.contentGeneration,
                TREE64_PUBLISH_CHUNK_VALID |
                    select(
                        0u,
                        TREE64_PUBLISH_CHUNK_OCCUPIED,
                        occupied));
        }
    }
    directory[record.directoryIndex] = descriptor;
}
