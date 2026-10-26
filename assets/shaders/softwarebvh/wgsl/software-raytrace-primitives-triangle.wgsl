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
    _padding0: u32,
};

@group(0) @binding(4) var<storage, read> blasMetadata: array<TriangleBLASMetadata>;
@group(0) @binding(5) var<storage, read> blasPrimitiveRemap: array<u32>;
@group(0) @binding(6) var<storage, read> blasTriangleTransforms: array<TriBaryTransform>;

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

    let originalPrimitiveIndex = blasPrimitiveRemap[triangleMetadata.primitiveRemapOffset + localPrimitiveIndex];
    let transform = blasTriangleTransforms[triangleMetadata.triangleTransformOffset + localPrimitiveIndex];
    let hit = intersect_triangle_transform(localOrigin, localDirection, transform);
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

    let transform = blasTriangleTransforms[triangleMetadata.triangleTransformOffset + localPrimitiveIndex];
    let hit = intersect_triangle_transform(localOrigin, localDirection, transform);
    if hit.z < 0.0 {
        return false;
    }

    let worldT = hit.z;
    return worldT >= uniforms.tMin && worldT <= maxRayT;
}
