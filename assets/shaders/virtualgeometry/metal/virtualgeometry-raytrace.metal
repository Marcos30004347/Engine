#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace raytracing;

struct InstanceInput { packed_float3 transform[4]; uint handle; uint mask; };
// Matches MTLIndirectAccelerationStructureInstanceDescriptor. CPU static asserts
// and GPU transform tests protect this ABI (72-byte stride, 8-byte alignment).
struct NativeInstance {
    packed_float3 transform[4]; uint options; uint mask;
    uint intersectionFunctionTableOffset; uint userID; ulong resourceID;
};
// uint3 aligns to 16; use an explicit scalar-array ABI for the 48-byte CPU ray.
struct RayInput { packed_float3 origin; float minDistance; packed_float3 direction;
    float maxDistance; uint mask; uint pad0; uint pad1; uint pad2; };
struct Bounds { packed_float3 lo; float pad0; packed_float3 hi; float pad1; };
struct Limits { uint maxInstances; uint maxRays; uint handles; uint threads;
    uint lightCull; uint maxVolumes; };

kernel void vgRtPrepareTrace(const device uint &rayCount [[buffer(1)]],
                             device uint *stats [[buffer(2)]],
                             constant Limits &limits [[buffer(3)]]) {
    stats[1] = uint(rayCount > limits.maxRays);
    stats[2] = min(rayCount, limits.maxRays);
    stats[7] = (stats[2] + limits.threads - 1) / limits.threads;
    stats[8] = 1; stats[9] = 1;
}

kernel void vgRtPrepare(const device uint &instanceCount [[buffer(0)]],
                        const device uint &rayCount [[buffer(1)]],
                        device uint *stats [[buffer(2)]],
                        constant Limits &limits [[buffer(3)]]) {
    stats[0] = limits.lightCull ? 0 : max(1u, min(instanceCount, limits.maxInstances));
    stats[1] = uint(instanceCount > limits.maxInstances || rayCount > limits.maxRays);
    stats[2] = min(rayCount, limits.maxRays); stats[3] = instanceCount;
    stats[4] = (max(1u,min(instanceCount,limits.maxInstances)) + limits.threads - 1) / limits.threads;
    stats[5] = 1; stats[6] = 1;
    stats[7] = (stats[2] + limits.threads - 1) / limits.threads;
    stats[8] = 1; stats[9] = 1;
}

kernel void vgRtInstances(const device InstanceInput *input [[buffer(0)]],
                          const device ulong *ids [[buffer(1)]],
                          device NativeInstance *output [[buffer(2)]],
                          device atomic_uint *stats [[buffer(3)]],
                          constant Limits &limits [[buffer(4)]],
                          const device Bounds *casters [[buffer(5)]],
                          const device Bounds *volumes [[buffer(6)]],
                          const device uint &volumeCount [[buffer(7)]],
                          uint i [[thread_position_in_grid]]) {
    const uint actual = atomic_load_explicit(&stats[3], memory_order_relaxed);
    if (i >= max(1u,min(actual,limits.maxInstances))) return;
    if (limits.lightCull) {
        if (i >= actual) return;
        bool contributes = false;
        const auto b = casters[i];
        if (!all(isfinite(float3(b.lo))) || !all(isfinite(float3(b.hi))) || any(float3(b.lo)>float3(b.hi)) || volumeCount > limits.maxVolumes) {
            atomic_store_explicit(&stats[1],1,memory_order_relaxed); contributes=true;
        }
        for (uint v=0;v<min(volumeCount,limits.maxVolumes);++v) {
            const auto volume=volumes[v];
            if (!all(isfinite(float3(volume.lo))) || !all(isfinite(float3(volume.hi))) || any(float3(volume.lo)>float3(volume.hi))) {
                atomic_store_explicit(&stats[1],1,memory_order_relaxed); contributes=true;
            }
            contributes |= all(float3(b.hi)>=float3(volume.lo)) && all(float3(volume.hi)>=float3(b.lo));
        }
        if (!contributes) return;
    }
    NativeInstance record;
    record.transform[0] = packed_float3(1,0,0);
    record.transform[1] = packed_float3(0,1,0);
    record.transform[2] = packed_float3(0,0,1);
    record.transform[3] = packed_float3(0,0,0);
    record.options = 0; record.mask = 0; record.intersectionFunctionTableOffset = 0;
    record.userID = i; record.resourceID = ids[0];
    // handle ~0 with mask 0 is a cluster installed without a BLAS (budget
    // full): deliberately no caster, not a broken scene.
    if (i < actual && !(input[i].handle == 0xffffffffu && input[i].mask == 0u)) {
        const auto candidate = input[i];
        const bool valid = candidate.handle > 0 && candidate.handle < limits.handles;
        const ulong id = valid ? ids[candidate.handle] : 0;
        bool finite = true;
        for (uint c=0; c<4; ++c) finite &= all(isfinite(float3(candidate.transform[c])));
        const float det=determinant(float3x3(float3(candidate.transform[0]),float3(candidate.transform[1]),float3(candidate.transform[2])));
        finite &= isfinite(det) && det!=0;
        if (id != 0 && finite) {
            for (uint c=0; c<4; ++c) record.transform[c] = candidate.transform[c];
            record.mask = candidate.mask; record.resourceID = id;
        } else atomic_store_explicit(&stats[1], 1, memory_order_relaxed);
    }
    const uint outputIndex=limits.lightCull ? atomic_fetch_add_explicit(&stats[0],1,memory_order_relaxed) : i;
    output[outputIndex] = record;
}

kernel void vgRtFinalizeInstances(device NativeInstance *output [[buffer(0)]],
                                  device uint *stats [[buffer(1)]],
                                  const device ulong *ids [[buffer(2)]]) {
    if (stats[0]!=0) return;
    NativeInstance record;
    record.transform[0]=packed_float3(1,0,0); record.transform[1]=packed_float3(0,1,0);
    record.transform[2]=packed_float3(0,0,1); record.transform[3]=packed_float3(0,0,0);
    record.options=0; record.mask=0; record.intersectionFunctionTableOffset=0;
    record.userID=0; record.resourceID=ids[0]; output[0]=record; stats[0]=1;
}

kernel void vgRtShadow(const device RayInput *rays [[buffer(0)]],
                       device uint *results [[buffer(1)]],
                       const device uint *stats [[buffer(2)]],
                       instance_acceleration_structure scene [[buffer(3)]],
                       uint i [[thread_position_in_grid]]) {
    if (i >= stats[2]) return;
    if (stats[1] != 0) { results[i] = 2; return; }
    const auto input = rays[i];
    if (!all(isfinite(float3(input.origin))) || !all(isfinite(float3(input.direction))) ||
        !isfinite(input.minDistance) || !isfinite(input.maxDistance) ||
        input.minDistance < 0 || input.maxDistance <= input.minDistance ||
        dot(float3(input.direction),float3(input.direction)) == 0) {
        results[i] = 2; return;
    }
    ray r; r.origin = input.origin; r.direction = input.direction;
    r.min_distance = input.minDistance; r.max_distance = input.maxDistance;
    intersector<triangle_data, instancing> query;
    query.assume_geometry_type(geometry_type::triangle);
    query.force_opacity(forced_opacity::opaque);
    query.accept_any_intersection(true);
    const auto hit = query.intersect(r, scene, input.mask);
    results[i] = hit.type == intersection_type::none ? 0 : 1;
}

struct ClosestOutput { float distance; uint state; uint instance; uint primitive; };
kernel void vgRtClosest(const device RayInput *rays [[buffer(0)]],
                        device ClosestOutput *results [[buffer(1)]],
                        const device uint *stats [[buffer(2)]],
                        instance_acceleration_structure scene [[buffer(3)]],
                        uint i [[thread_position_in_grid]]) {
    if (i >= stats[2]) return;
    ClosestOutput output = {0, 2, ~0u, ~0u};
    const auto input = rays[i];
    if (stats[1] != 0 || !all(isfinite(float3(input.origin))) ||
        !all(isfinite(float3(input.direction))) || !isfinite(input.minDistance) ||
        !isfinite(input.maxDistance) || input.minDistance < 0 ||
        input.maxDistance <= input.minDistance || dot(float3(input.direction),float3(input.direction)) == 0) {
        results[i] = output; return;
    }
    ray r; r.origin=input.origin; r.direction=input.direction;
    r.min_distance=input.minDistance; r.max_distance=input.maxDistance;
    intersector<triangle_data, instancing> query;
    query.assume_geometry_type(geometry_type::triangle);
    query.force_opacity(forced_opacity::opaque);
    query.accept_any_intersection(false);
    const auto hit=query.intersect(r,scene,input.mask);
    output.state = hit.type == intersection_type::none ? 0 : 1;
    if (output.state == 1) {
        output.distance=hit.distance; output.instance=hit.instance_id; output.primitive=hit.primitive_id;
    }
    results[i]=output;
}

struct EncodedDecode { uint positionBase, indexBase, descriptorBase; float unitScale; uint vertices; };
uint vgRtBits(const device uint *words,uint base,uint offset,uint count) {
    if (count==0) return 0;
    uint value=words[base+offset/32]>>(offset%32);
    if (offset%32+count>32) value|=words[base+offset/32+1]<<(32-offset%32);
    return count==32 ? value : value&((1u<<count)-1);
}
kernel void vgRtDecodeEncoded(const device uint *words [[buffer(0)]],
                               device packed_float3 *vertices [[buffer(1)]],
                               constant EncodedDecode &decode [[buffer(2)]],
                               uint i [[thread_position_in_grid]]) {
    if (i>=decode.vertices) return;
    const device uint *d=words+decode.descriptorBase;
    uint vertexIndex=(words[decode.indexBase+d[6]+i/4]>>((i%4)*8))&255;
    uint3 bits=uint3(32-clz(d[10]),32-clz(d[11]),32-clz(d[12]));
    uint bit=vertexIndex*(bits.x+bits.y+bits.z);
    float3 q;
    q.x=as_type<float>(d[14])+float(vgRtBits(words,decode.positionBase+d[0],bit,bits.x)); bit+=bits.x;
    q.y=as_type<float>(d[15])+float(vgRtBits(words,decode.positionBase+d[0],bit,bits.y)); bit+=bits.y;
    q.z=as_type<float>(d[16])+float(vgRtBits(words,decode.positionBase+d[0],bit,bits.z));
    vertices[i]=packed_float3(q/(float(1u<<d[13])*decode.unitScale));
}
