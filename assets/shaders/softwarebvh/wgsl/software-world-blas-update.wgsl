struct DispatchIndirectArgs {
    groupCountX: u32,
    groupCountY: u32,
    groupCountZ: u32,
};

struct Uniforms {
    instanceRangeCount: u32,
    triangleUpdateItemCount: u32,
    bvh8UpdateItemCount: u32,
    leafRemapUpdateItemCount: u32,
    workgroupSize: u32,
    enabled: u32,
    _padding0: vec2<u32>,
};

struct TriBaryTransformGPU {
    row0: vec4<f32>,
    row1: vec4<f32>,
    row2: vec4<f32>,
};

struct InstanceTransformGPU {
    transform0: vec4<f32>,
    transform1: vec4<f32>,
    transform2: vec4<f32>,
    transform3: vec4<f32>,
    inverseTransform0: vec4<f32>,
    inverseTransform1: vec4<f32>,
    inverseTransform2: vec4<f32>,
    inverseTransform3: vec4<f32>,
};

struct WorldInstanceRangeGPU {
    instanceIndex: u32,
    sceneInstanceId: u32,
    blasHandle: u32,
    flags: u32,
    worldCWBVHNodeOffset: u32,
    worldCWBVHNodeCount: u32,
    worldBVH8NodeOffset: u32,
    worldBVH8NodeCount: u32,
    worldTriangleOffset: u32,
    worldTriangleCount: u32,
    worldLeafRemapOffset: u32,
    worldLeafRemapCount: u32,
    valid: u32,
    dynamic: u32,
    needsUpdate: u32,
    primitiveRemapOffset: u32,
};

struct UpdateCountersGPU {
    updateInstanceCount: atomic<u32>,
    updateTriangleCount: atomic<u32>,
    updateBVH8NodeCount: atomic<u32>,
    updateCWBVHNodeCount: atomic<u32>,
    updateLeafRemapCount: atomic<u32>,
};

struct TriangleUpdateItemGPU {
    sourceTriangleIndex: u32,
    worldTriangleIndex: u32,
    instanceRangeIndex: u32,
    _padding0: u32,
};

struct BVH8UpdateItemGPU {
    sourceNodeIndex: u32,
    worldNodeIndex: u32,
    instanceRangeIndex: u32,
    worldCWBVHNodeOffset: u32,
    worldBVH8NodeOffset: u32,
    worldLeafRemapOffset: u32,
    _padding0: u32,
    _padding1: u32,
};

struct LeafRemapUpdateItemGPU {
    sourceLeafRemapIndex: u32,
    worldLeafRemapIndex: u32,
    instanceRangeIndex: u32,
    _padding0: u32,
};

struct WorldLeafEntryGPU {
    worldTriangleIndex: u32,
    originalPrimitiveIndex: u32,
};

struct BVH8NodeGPU {
    boundsMin: vec3<f32>,
    cwbvhOutputIndex: u32,
    boundsMax: vec3<f32>,
    internalChildBase: u32,
    childMin: array<vec4<f32>, 8>,
    childMax: array<vec4<f32>, 8>,
    childIndex: array<u32, 8>,
    childMeta: array<u32, 8>,
    leafBase: u32,
    validChildMask: u32,
    internalChildMask: u32,
    leafChildMask: u32,
};

struct PackedCWBVHNodeGPU {
    orxy: vec2<u32>,
    oeim: vec2<u32>,
    ntbi: vec2<u32>,
    info: vec2<u32>,
    qlox: vec2<u32>,
    qloy: vec2<u32>,
    qloz: vec2<u32>,
    qhix: vec2<u32>,
    qhiy: vec2<u32>,
    qhiz: vec2<u32>,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var<storage, read> instanceRanges: array<WorldInstanceRangeGPU>;
@group(0) @binding(2) var<storage, read_write> updateInstanceRangeIndices: array<u32>;
@group(0) @binding(3) var<storage, read_write> updateCounters: UpdateCountersGPU;
@group(0) @binding(4) var<storage, read_write> dispatchArgs: array<DispatchIndirectArgs>;
@group(0) @binding(5) var<storage, read> triangleUpdateItems: array<TriangleUpdateItemGPU>;
@group(0) @binding(6) var<storage, read> bvh8UpdateItems: array<BVH8UpdateItemGPU>;
@group(0) @binding(7) var<storage, read> leafRemapUpdateItems: array<LeafRemapUpdateItemGPU>;
@group(0) @binding(8) var<storage, read> instanceTransforms: array<InstanceTransformGPU>;
@group(0) @binding(9) var<storage, read> sourceTriangles: array<TriBaryTransformGPU>;
@group(0) @binding(10) var<storage, read_write> worldTriangles: array<TriBaryTransformGPU>;
@group(0) @binding(11) var<storage, read> sourceBVH8Nodes: array<BVH8NodeGPU>;
@group(0) @binding(12) var<storage, read_write> worldBVH8Nodes: array<BVH8NodeGPU>;
@group(0) @binding(13) var<storage, read> sourceLeafRemap: array<u32>;
@group(0) @binding(14) var<storage, read_write> worldLeafRemap: array<WorldLeafEntryGPU>;
@group(0) @binding(15) var<storage, read_write> sceneCWBVHNodes: array<PackedCWBVHNodeGPU>;
@group(0) @binding(16) var<storage, read> sourcePrimitiveRemap: array<u32>;

fn dispatch_groups(itemCount: u32) -> u32 {
    let divisor = max(uniforms.workgroupSize, 1u);
    return select((itemCount + divisor - 1u) / divisor, 0u, itemCount == 0u);
}

fn pack4xU8(x: u32, y: u32, z: u32, w: u32) -> u32 {
    return (x & 0xffu) | ((y & 0xffu) << 8u) | ((z & 0xffu) << 16u) | ((w & 0xffu) << 24u);
}

fn decode_signed_u8(value: u32) -> i32 {
    return select(i32(value), i32(value) - 256, value >= 128u);
}

fn encode_signed_exponent8(extent: f32) -> u32 {
    if !(extent > 0.0) {
        return 0u;
    }

    var exponent = clamp(i32(ceil(log2(extent / 255.0))), -128, 127);
    let scale = exp2(f32(exponent));
    if exponent < 127 && ceil(extent / scale) > 255.0 {
        exponent += 1;
    }
    return select(u32(exponent), u32(exponent + 256), exponent < 0) & 0xffu;
}

fn decode_exponent_scale(encodedExponent: u32) -> f32 {
    return exp2(f32(decode_signed_u8(encodedExponent & 0xffu)));
}

fn quantize_lower_bound(value: f32, parentMin: f32, scale: f32) -> u32 {
    if !(scale > 0.0) {
        return 0u;
    }
    var q = u32(clamp(floor((value - parentMin) / scale), 0.0, 255.0));
    if q > 0u {
        q -= 1u;
    }
    return q;
}

fn quantize_upper_bound(value: f32, parentMin: f32, scale: f32) -> u32 {
    if !(scale > 0.0) {
        return 0u;
    }
    var q = u32(clamp(ceil((value - parentMin) / scale), 0.0, 255.0));
    if q < 255u {
        q += 1u;
    }
    return q;
}

@compute @workgroup_size(1, 1, 1)
fn clear_update_counters(@builtin(global_invocation_id) globalId: vec3<u32>) {
    if globalId.x != 0u {
        return;
    }
    atomicStore(&updateCounters.updateInstanceCount, 0u);
    atomicStore(&updateCounters.updateTriangleCount, 0u);
    atomicStore(&updateCounters.updateBVH8NodeCount, 0u);
    atomicStore(&updateCounters.updateCWBVHNodeCount, 0u);
    atomicStore(&updateCounters.updateLeafRemapCount, 0u);
    dispatchArgs[0u] = DispatchIndirectArgs(0u, 1u, 1u);
    dispatchArgs[1u] = DispatchIndirectArgs(0u, 1u, 1u);
    dispatchArgs[2u] = DispatchIndirectArgs(0u, 1u, 1u);
    dispatchArgs[3u] = DispatchIndirectArgs(0u, 1u, 1u);
}

@compute @workgroup_size(64, 1, 1)
fn prepare_update_jobs(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let rangeIndex = globalId.x;
    if rangeIndex >= uniforms.instanceRangeCount {
        return;
    }
    if uniforms.enabled == 0u {
        return;
    }

    let range = instanceRanges[rangeIndex];
    if range.valid == 0u || range.needsUpdate == 0u {
        return;
    }

    let jobIndex = atomicAdd(&updateCounters.updateInstanceCount, 1u);
    updateInstanceRangeIndices[jobIndex] = rangeIndex;
    atomicAdd(&updateCounters.updateTriangleCount, range.worldTriangleCount);
    atomicAdd(&updateCounters.updateBVH8NodeCount, range.worldBVH8NodeCount);
    atomicAdd(&updateCounters.updateCWBVHNodeCount, range.worldCWBVHNodeCount);
    atomicAdd(&updateCounters.updateLeafRemapCount, range.worldLeafRemapCount);
}

@compute @workgroup_size(1, 1, 1)
fn prepare_update_dispatches(@builtin(global_invocation_id) globalId: vec3<u32>) {
    if globalId.x != 0u {
        return;
    }
    dispatchArgs[0u] = DispatchIndirectArgs(dispatch_groups(atomicLoad(&updateCounters.updateTriangleCount)), 1u, 1u);
    dispatchArgs[1u] = DispatchIndirectArgs(dispatch_groups(atomicLoad(&updateCounters.updateBVH8NodeCount)), 1u, 1u);
    dispatchArgs[2u] = DispatchIndirectArgs(dispatch_groups(atomicLoad(&updateCounters.updateLeafRemapCount)), 1u, 1u);
    dispatchArgs[3u] = DispatchIndirectArgs(dispatch_groups(atomicLoad(&updateCounters.updateBVH8NodeCount)), 1u, 1u);
}

fn transform_plane(row: vec4<f32>, instance: InstanceTransformGPU) -> vec4<f32> {
    return vec4<f32>(
        dot(row, instance.inverseTransform0),
        dot(row, instance.inverseTransform1),
        dot(row, instance.inverseTransform2),
        dot(row, instance.inverseTransform3));
}

@compute @workgroup_size(64, 1, 1)
fn transform_triangles(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let itemIndex = globalId.x;
    if itemIndex >= uniforms.triangleUpdateItemCount {
        return;
    }
    if uniforms.enabled == 0u {
        return;
    }

    let item = triangleUpdateItems[itemIndex];
    let instanceRange = instanceRanges[item.instanceRangeIndex];
    let instance = instanceTransforms[instanceRange.instanceIndex];
    let source = sourceTriangles[item.sourceTriangleIndex];
    var output: TriBaryTransformGPU;
    output.row0 = transform_plane(source.row0, instance);
    output.row1 = transform_plane(source.row1, instance);
    output.row2 = transform_plane(source.row2, instance);
    worldTriangles[item.worldTriangleIndex] = output;
}

fn transform_point(point: vec3<f32>, instance: InstanceTransformGPU) -> vec3<f32> {
    return instance.transform0.xyz * point.x +
        instance.transform1.xyz * point.y +
        instance.transform2.xyz * point.z +
        instance.transform3.xyz;
}

fn transform_extent(extent: vec3<f32>, instance: InstanceTransformGPU) -> vec3<f32> {
    return abs(instance.transform0.xyz) * extent.x +
        abs(instance.transform1.xyz) * extent.y +
        abs(instance.transform2.xyz) * extent.z;
}

fn transform_aabb(minPoint: vec3<f32>, maxPoint: vec3<f32>, instance: InstanceTransformGPU) -> array<vec3<f32>, 2> {
    let center = (minPoint + maxPoint) * 0.5;
    let extent = max(maxPoint - center, vec3<f32>(0.0));
    let worldCenter = transform_point(center, instance);
    let worldExtent = transform_extent(extent, instance);
    var result: array<vec3<f32>, 2>;
    result[0] = worldCenter - worldExtent;
    result[1] = worldCenter + worldExtent;
    return result;
}

@compute @workgroup_size(64, 1, 1)
fn refit_world_bvh8(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let itemIndex = globalId.x;
    if itemIndex >= uniforms.bvh8UpdateItemCount {
        return;
    }
    if uniforms.enabled == 0u {
        return;
    }

    let item = bvh8UpdateItems[itemIndex];
    let instanceRange = instanceRanges[item.instanceRangeIndex];
    let instance = instanceTransforms[instanceRange.instanceIndex];
    let sourceIndex = item.sourceNodeIndex;
    var output = BVH8NodeGPU();

    let bounds = transform_aabb(sourceBVH8Nodes[sourceIndex].boundsMin, sourceBVH8Nodes[sourceIndex].boundsMax, instance);
    output.boundsMin = bounds[0];
    output.boundsMax = bounds[1];
    output.cwbvhOutputIndex = item.worldCWBVHNodeOffset + sourceBVH8Nodes[sourceIndex].cwbvhOutputIndex;
    output.internalChildBase = select(0xffffffffu, item.worldCWBVHNodeOffset + sourceBVH8Nodes[sourceIndex].internalChildBase, sourceBVH8Nodes[sourceIndex].internalChildBase != 0xffffffffu);
    output.leafBase = item.worldLeafRemapOffset + sourceBVH8Nodes[sourceIndex].leafBase;
    output.validChildMask = sourceBVH8Nodes[sourceIndex].validChildMask;
    output.internalChildMask = sourceBVH8Nodes[sourceIndex].internalChildMask;
    output.leafChildMask = sourceBVH8Nodes[sourceIndex].leafChildMask;

    for (var childSlot = 0u; childSlot < 8u; childSlot += 1u) {
        output.childIndex[childSlot] = sourceBVH8Nodes[sourceIndex].childIndex[childSlot];
        output.childMeta[childSlot] = sourceBVH8Nodes[sourceIndex].childMeta[childSlot];
        if (sourceBVH8Nodes[sourceIndex].childMeta[childSlot] == 0u) {
            continue;
        }
        let childBounds = transform_aabb(sourceBVH8Nodes[sourceIndex].childMin[childSlot].xyz, sourceBVH8Nodes[sourceIndex].childMax[childSlot].xyz, instance);
        output.childMin[childSlot] = vec4<f32>(childBounds[0], 0.0);
        output.childMax[childSlot] = vec4<f32>(childBounds[1], 0.0);

        let isLeaf = (sourceBVH8Nodes[sourceIndex].childMeta[childSlot] & 2u) != 0u;
        if isLeaf {
            output.childIndex[childSlot] = item.worldLeafRemapOffset + sourceBVH8Nodes[sourceIndex].childIndex[childSlot];
        } else {
            output.childIndex[childSlot] = item.worldBVH8NodeOffset + sourceBVH8Nodes[sourceIndex].childIndex[childSlot];
        }
    }

    worldBVH8Nodes[item.worldNodeIndex] = output;
}

@compute @workgroup_size(64, 1, 1)
fn copy_leaf_remap(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let itemIndex = globalId.x;
    if itemIndex >= uniforms.leafRemapUpdateItemCount {
        return;
    }
    if uniforms.enabled == 0u {
        return;
    }

    let item = leafRemapUpdateItems[itemIndex];
    let range = instanceRanges[item.instanceRangeIndex];
    let localPrimitiveIndex = sourceLeafRemap[item.sourceLeafRemapIndex];
    worldLeafRemap[item.worldLeafRemapIndex] = WorldLeafEntryGPU(
        range.worldTriangleOffset + localPrimitiveIndex,
        sourcePrimitiveRemap[range.primitiveRemapOffset + localPrimitiveIndex]);
}

@compute @workgroup_size(64, 1, 1)
fn compress_bvh8_to_cwbvh(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let itemIndex = globalId.x;
    if itemIndex >= uniforms.bvh8UpdateItemCount {
        return;
    }
    if uniforms.enabled == 0u {
        return;
    }

    let item = bvh8UpdateItems[itemIndex];
    let sourceIndex = item.worldNodeIndex;
    let cwbvhOutputIndex = worldBVH8Nodes[sourceIndex].cwbvhOutputIndex;
    if cwbvhOutputIndex == 0xffffffffu {
        return;
    }

    let parentMin = worldBVH8Nodes[sourceIndex].boundsMin;
    let parentMax = worldBVH8Nodes[sourceIndex].boundsMax;
    let sxExp = encode_signed_exponent8(parentMax.x - parentMin.x);
    let syExp = encode_signed_exponent8(parentMax.y - parentMin.y);
    let szExp = encode_signed_exponent8(parentMax.z - parentMin.z);
    let scale = vec3<f32>(
        decode_exponent_scale(sxExp),
        decode_exponent_scale(syExp),
        decode_exponent_scale(szExp));

    var node = PackedCWBVHNodeGPU();
    var imask = 0u;

    for (var childSlot = 0u; childSlot < 8u; childSlot += 1u) {
        if ((worldBVH8Nodes[sourceIndex].childMeta[childSlot] & 1u) == 0u) {
            continue;
        }

        let arrayIndex = childSlot >> 2u;
        let bitOffset = (childSlot & 3u) * 8u;
        node.qlox[arrayIndex] |= quantize_lower_bound(worldBVH8Nodes[sourceIndex].childMin[childSlot].x, parentMin.x, scale.x) << bitOffset;
        node.qloy[arrayIndex] |= quantize_lower_bound(worldBVH8Nodes[sourceIndex].childMin[childSlot].y, parentMin.y, scale.y) << bitOffset;
        node.qloz[arrayIndex] |= quantize_lower_bound(worldBVH8Nodes[sourceIndex].childMin[childSlot].z, parentMin.z, scale.z) << bitOffset;
        node.qhix[arrayIndex] |= quantize_upper_bound(worldBVH8Nodes[sourceIndex].childMax[childSlot].x, parentMin.x, scale.x) << bitOffset;
        node.qhiy[arrayIndex] |= quantize_upper_bound(worldBVH8Nodes[sourceIndex].childMax[childSlot].y, parentMin.y, scale.y) << bitOffset;
        node.qhiz[arrayIndex] |= quantize_upper_bound(worldBVH8Nodes[sourceIndex].childMax[childSlot].z, parentMin.z, scale.z) << bitOffset;

        let isLeaf = (worldBVH8Nodes[sourceIndex].childMeta[childSlot] & 2u) != 0u;
        if isLeaf {
            let leafOffset = worldBVH8Nodes[sourceIndex].childIndex[childSlot] - worldBVH8Nodes[sourceIndex].leafBase;
            node.info[arrayIndex] |= ((1u << 5u) | (leafOffset & 0x1fu)) << bitOffset;
        } else {
            imask |= 1u << childSlot;
            node.info[arrayIndex] |= ((1u << 5u) | ((childSlot + 24u) & 0x1fu)) << bitOffset;
        }
    }

    node.orxy = vec2<u32>(bitcast<u32>(parentMin.x), bitcast<u32>(parentMin.y));
    node.oeim = vec2<u32>(bitcast<u32>(parentMin.z), pack4xU8(sxExp, syExp, szExp, imask));
    node.ntbi = vec2<u32>(select(0u, worldBVH8Nodes[sourceIndex].internalChildBase, worldBVH8Nodes[sourceIndex].internalChildBase != 0xffffffffu), worldBVH8Nodes[sourceIndex].leafBase);
    sceneCWBVHNodes[cwbvhOutputIndex] = node;
}
