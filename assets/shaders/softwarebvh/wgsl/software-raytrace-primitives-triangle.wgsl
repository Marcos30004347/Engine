struct TriBaryTransform {
    row0: vec4<f32>,
    row1: vec4<f32>,
    row2: vec4<f32>,
};

struct BLASMetadataCommon {
    handleId: u32,
    cwbvhNodeOffset: u32,
    cwbvhNodeCount: u32,
    leafRemapOffset: u32,
};

struct TriangleBLASMetadata {
    handleId: u32,
    cwbvhNodeOffset: u32,
    cwbvhNodeCount: u32,
    leafRemapOffset: u32,
    primitiveRemapOffset: u32,
    primitiveCount: u32,
    triangleTransformOffset: u32,
    vertexOffset: u32,
    indexOffset: u32,
    primitiveMaterialOffset: u32,
    trianglePayloadOffset: u32,
    trianglePayloadFlags: u32,
};

struct QuantizedTrianglePayload {
    q0: u32,
    q1: u32,
    q2_frame: u32,
};

const TRIANGLE_PAYLOAD_FLAG_QUANTIZED_U8_NODE_FRAME = 1u;

@group(0) @binding(4) var<storage, read> blasMetadata: array<TriangleBLASMetadata>;
@group(0) @binding(5) var<storage, read> blasPrimitiveRemap: array<u32>;
@group(0) @binding(6) var<storage, read> blasTriangleTransforms: array<TriBaryTransform>;
#ifndef SWRT_WORLD_CWBVH
@group(0) @binding(29) var<storage, read> blasQuantizedTrianglePayloads: array<QuantizedTrianglePayload>;
#endif

fn load_blas_metadata_common(blasHandle: u32) -> BLASMetadataCommon {
    let metadata = blasMetadata[blasHandle];
    return BLASMetadataCommon(metadata.handleId, metadata.cwbvhNodeOffset, metadata.cwbvhNodeCount, metadata.leafRemapOffset);
}

fn load_triangle_blas_metadata(blasHandle: u32) -> TriangleBLASMetadata {
    return blasMetadata[blasHandle];
}

fn load_trace_primitive_metadata(blasHandle: u32) -> TriangleBLASMetadata {
    return load_triangle_blas_metadata(blasHandle);
}

fn intersect_triangle_transform(rayOriginLocal: vec3<f32>, rayDirLocal: vec3<f32>, transform: TriBaryTransform) -> vec3<f32> {
    let oz = -(dot(transform.row2.xyz, rayOriginLocal) + transform.row2.w);
    let dz = dot(transform.row2.xyz, rayDirLocal);
    if abs(dz) < 1.0e-8 {
        return vec3<f32>(-1.0);
    }
    let t = oz / dz;
    if t <= 0.0 {
        return vec3<f32>(-1.0);
    }

    let ox = dot(transform.row0.xyz, rayOriginLocal) + transform.row0.w;
    let dx = dot(transform.row0.xyz, rayDirLocal);
    let u = ox + t * dx;
    if u < 0.0 || u > 1.0 {
        return vec3<f32>(-1.0);
    }

    let oy = dot(transform.row1.xyz, rayOriginLocal) + transform.row1.w;
    let dy = dot(transform.row1.xyz, rayDirLocal);
    let v = oy + t * dy;
    if v < 0.0 || (u + v) > 1.0 {
        return vec3<f32>(-1.0);
    }

    return vec3<f32>(u, v, t);
}

fn unpack_u8(value: u32, lane: u32) -> u32 {
    return (value >> (lane * 8u)) & 0xffu;
}

fn triangle_payload_frame_node_index(payload: QuantizedTrianglePayload) -> u32 {
    return (payload.q2_frame >> 8u) & 0x00ffffffu;
}

fn decode_quantized_triangle_position(q: vec3<u32>, frameNode: vec4<u32>) -> vec3<f32> {
    let origin = vec3<f32>(bitcast<f32>(frameNode.x), bitcast<f32>(frameNode.y), bitcast<f32>(frameNode.z));
    let e_imask = unpack4xU8(frameNode.w);
    let scale = vec3<f32>(
        bitcast<f32>(u32(decode_signed_u8(e_imask.x) + 127) << 23u),
        bitcast<f32>(u32(decode_signed_u8(e_imask.y) + 127) << 23u),
        bitcast<f32>(u32(decode_signed_u8(e_imask.z) + 127) << 23u));
    return origin + vec3<f32>(q) * scale;
}

fn intersect_triangle_vertices(rayOriginLocal: vec3<f32>, rayDirLocal: vec3<f32>, v0: vec3<f32>, v1: vec3<f32>, v2: vec3<f32>) -> vec3<f32> {
    let edge1 = v1 - v0;
    let edge2 = v2 - v0;
    let pvec = cross(rayDirLocal, edge2);
    let det = dot(edge1, pvec);
    if abs(det) < 1.0e-8 {
        return vec3<f32>(-1.0);
    }

    let invDet = 1.0 / det;
    let tvec = rayOriginLocal - v0;
    let u = dot(tvec, pvec) * invDet;
    if u < 0.0 || u > 1.0 {
        return vec3<f32>(-1.0);
    }

    let qvec = cross(tvec, edge1);
    let v = dot(rayDirLocal, qvec) * invDet;
    if v < 0.0 || (u + v) > 1.0 {
        return vec3<f32>(-1.0);
    }

    let t = dot(edge2, qvec) * invDet;
    if t <= 0.0 {
        return vec3<f32>(-1.0);
    }

    return vec3<f32>(u, v, t);
}

fn intersect_triangle_payload(rayOriginLocal: vec3<f32>, rayDirLocal: vec3<f32>, metadata: TriangleBLASMetadata, localPrimitiveIndex: u32) -> vec3<f32> {
#ifndef SWRT_WORLD_CWBVH
    if (metadata.trianglePayloadFlags & TRIANGLE_PAYLOAD_FLAG_QUANTIZED_U8_NODE_FRAME) != 0u {
        let payload = blasQuantizedTrianglePayloads[metadata.trianglePayloadOffset + localPrimitiveIndex];
        let frameNodeIndex = triangle_payload_frame_node_index(payload);
        let frameNode = blasNodes[frameNodeIndex * 5u];
        let v0 = decode_quantized_triangle_position(
            vec3<u32>(
                unpack_u8(payload.q0, 0u),
                unpack_u8(payload.q0, 1u),
                unpack_u8(payload.q0, 2u)),
            frameNode);
        let v1 = decode_quantized_triangle_position(
            vec3<u32>(
                unpack_u8(payload.q0, 3u),
                unpack_u8(payload.q1, 0u),
                unpack_u8(payload.q1, 1u)),
            frameNode);
        let v2 = decode_quantized_triangle_position(
            vec3<u32>(
                unpack_u8(payload.q1, 2u),
                unpack_u8(payload.q1, 3u),
                unpack_u8(payload.q2_frame, 0u)),
            frameNode);
        return intersect_triangle_vertices(rayOriginLocal, rayDirLocal, v0, v1, v2);
    }
#endif
    let transform = blasTriangleTransforms[metadata.triangleTransformOffset + localPrimitiveIndex];
    return intersect_triangle_transform(rayOriginLocal, rayDirLocal, transform);
}

fn trace_blas_primitive_leaf_local(
    rayOrigin: vec3<f32>,
    rayDirection: vec3<f32>,
    instance: InstanceGPU,
    instanceIndex: u32,
    metadata: BLASMetadataCommon,
    localPrimitiveIndex: u32,
    localOrigin: vec3<f32>,
    localDirection: vec3<f32>,
    state: ptr<function, HitState>) {
    trace_blas_primitive_leaf_local_cached(
        rayOrigin,
        rayDirection,
        instance,
        instanceIndex,
        metadata,
        load_trace_primitive_metadata(instance.blasHandle),
        localPrimitiveIndex,
        localOrigin,
        localDirection,
        vec3<f32>(0.0),
        state);
}

fn trace_blas_primitive_leaf_local_cached(
    rayOrigin: vec3<f32>,
    rayDirection: vec3<f32>,
    instance: InstanceGPU,
    instanceIndex: u32,
    metadata: BLASMetadataCommon,
    triangleMetadata: TriangleBLASMetadata,
    localPrimitiveIndex: u32,
    localOrigin: vec3<f32>,
    localDirection: vec3<f32>,
    localInvDir: vec3<f32>,
    state: ptr<function, HitState>) {
    (*state).iterationCount += 1u;
    if localPrimitiveIndex >= triangleMetadata.primitiveCount {
        return;
    }

    trace_stat_add_triangle_test(1u);
    let originalPrimitiveIndex = blasPrimitiveRemap[triangleMetadata.primitiveRemapOffset + localPrimitiveIndex];
    let hit = intersect_triangle_payload(localOrigin, localDirection, triangleMetadata, localPrimitiveIndex);
    if hit.z < 0.0 {
        return;
    }

    let worldT = hit.z;
    if worldT < uniforms.tMin || worldT > uniforms.tMax || worldT >= (*state).bestT {
        return;
    }

    update_best_hit_local(state, worldT, hit.xy, originalPrimitiveIndex, instanceIndex);
}

fn trace_blas_primitive_leaf_any_hit(
    rayOrigin: vec3<f32>,
    rayDirection: vec3<f32>,
    instance: InstanceGPU,
    metadata: BLASMetadataCommon,
    localPrimitiveIndex: u32,
    localOrigin: vec3<f32>,
    localDirection: vec3<f32>,
    maxRayT: f32) -> bool {
    return trace_blas_primitive_leaf_any_hit_cached(
        rayOrigin,
        rayDirection,
        instance,
        metadata,
        load_trace_primitive_metadata(instance.blasHandle),
        localPrimitiveIndex,
        localOrigin,
        localDirection,
        vec3<f32>(0.0),
        maxRayT);
}

fn trace_blas_primitive_leaf_any_hit_cached(
    rayOrigin: vec3<f32>,
    rayDirection: vec3<f32>,
    instance: InstanceGPU,
    metadata: BLASMetadataCommon,
    triangleMetadata: TriangleBLASMetadata,
    localPrimitiveIndex: u32,
    localOrigin: vec3<f32>,
    localDirection: vec3<f32>,
    localInvDir: vec3<f32>,
    maxRayT: f32) -> bool {
    if localPrimitiveIndex >= triangleMetadata.primitiveCount {
        return false;
    }

    let hit = intersect_triangle_payload(localOrigin, localDirection, triangleMetadata, localPrimitiveIndex);
    if hit.z < 0.0 {
        return false;
    }

    let worldT = hit.z;
    return worldT >= uniforms.tMin && worldT <= maxRayT;
}
