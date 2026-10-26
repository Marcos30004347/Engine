#include "vsm-common.wgsl"
struct Config { pageResolution: u32, pageSize: u32, atlasResolution: u32, capacity: u32,
    reverseZ: u32, rayBudget: u32, p0: u32, p1: u32, }
struct Cascade { view: mat4x4<f32>, proj: mat4x4<f32>, viewProj: mat4x4<f32>,
    worldExtent: f32, pageWorldSize: f32, lightIndex: u32, cascadeIndex: u32,
    pageTableResolution: u32, lightType: u32, faceIndex: u32, mipLevel: u32, }
struct CascadeState { pageOffset: vec2<i32>, pageShift: vec2<i32>, padding: vec4<u32>, }
struct Ray { origin: vec3<f32>, minDistance: f32, direction: vec3<f32>, maxDistance: f32,
    mask: u32, p0: u32, p1: u32, p2: u32, }
struct Hit { distance: f32, state: u32, instance: u32, primitive: u32, }
@group(0) @binding(0) var<uniform> config: Config;
@group(0) @binding(1) var<storage,read> counters: array<u32>;
@group(0) @binding(2) var<storage,read> coarse: array<u32>;
@group(0) @binding(3) var<storage,read> fine: array<u32>;
@group(0) @binding(4) var<storage,read_write> vpt: array<atomic<u32>>;
@group(0) @binding(5) var<storage,read> matrices: array<Cascade>;
@group(0) @binding(6) var<storage,read> states: array<CascadeState>;
@group(0) @binding(7) var<storage,read_write> jobs: array<vec2<u32>>; // packed virtual page, physical entry snapshot
@group(0) @binding(8) var<storage,read_write> rayCount: u32;
@group(0) @binding(9) var<storage,read_write> rays: array<Ray>;
@group(0) @binding(10) var<storage,read> hits: array<Hit>;
@group(0) @binding(11) var<storage,read_write> depths: array<f32>;
@group(0) @binding(12) var<storage,read_write> valid: array<atomic<u32>>;
@group(0) @binding(13) var<storage,read_write> dispatch: array<u32>;

fn page_index(packed: u32) -> u32 {
    let p=unpack_layered_coords(packed);
    return vsm_page_table_index(p.z,p.xy,config.pageResolution);
}
@compute @workgroup_size(1)
fn schedule_pages() {
    let pageTexels=config.pageSize*config.pageSize;
    let limit=min(config.capacity,config.rayBudget/pageTexels);
    var count=0u;
    let coarseCount=min(counters[1],arrayLength(&coarse));
    let fineCount=min(counters[0],arrayLength(&fine));
    for (var i=0u;i<coarseCount+fineCount && count<limit;i++) {
        var packed=0u;
        if (i<coarseCount) { packed=coarse[i]; } else { packed=fine[i-coarseCount]; }
        let coords=unpack_layered_coords(packed);
        let index=page_index(packed);
        if (index>=arrayLength(&vpt) || coords.z>=arrayLength(&matrices)) { continue; }
        let entry=atomicLoad(&vpt[index]);
        if (!get_is_allocated(entry) || !get_is_dirty(entry)) { continue; }
        jobs[count]=vec2<u32>(packed,entry);
        atomicStore(&valid[count],1u);
        count++;
    }
    rayCount=count*pageTexels;
    dispatch[0]=(rayCount+63u)/64u; dispatch[1]=1u; dispatch[2]=1u;
}
// Manager projections are orthographic or symmetric perspective, with an
// orthonormal look-at view. Solve that projection directly without matrix readback.
fn unproject(uv: vec2<f32>, depth: f32, m: Cascade) -> vec3<f32> {
    let ndc=uv*2.0-vec2<f32>(1.0);
    let p=m.proj;
    let z=(p[3].z-depth*p[3].w)/(depth*p[2].w-p[2].z);
    let w=p[2].w*z+p[3].w;
    let v=vec3<f32>((ndc.x*w-p[2].x*z-p[3].x)/p[0].x,
        (ndc.y*w-p[2].y*z-p[3].y)/p[1].y,z);
    let rotation=mat3x3<f32>(m.view[0].xyz,m.view[1].xyz,m.view[2].xyz);
    return transpose(rotation)*(v-m.view[3].xyz);
}
@compute @workgroup_size(64)
fn generate_rays(@builtin(global_invocation_id) id: vec3<u32>) {
    if (id.x>=rayCount) { return; }
    let pageTexels=config.pageSize*config.pageSize;
    let job=id.x/pageTexels; let pixel=id.x%pageTexels;
    let coords=unpack_layered_coords(jobs[job].x); let m=matrices[coords.z];
    let res=i32(m.pageTableResolution);
    let page=vec2<u32>(((vec2<i32>(coords.xy)-states[coords.z].pageOffset)%res+res)%res);
    let uv=(vec2<f32>(page*config.pageSize+vec2<u32>(pixel%config.pageSize,pixel/config.pageSize))+vec2<f32>(0.5))/f32(m.pageTableResolution*config.pageSize);
    let near=select(0.0,1.0,config.reverseZ!=0u);
    let start=unproject(uv,near,m); let end=unproject(uv,1.0-near,m);
    let delta=end-start; let length=length(delta);
    rays[id.x]=Ray(start,0.0,delta/max(length,1e-20),length,255u,0u,0u,0u);
    if (!(length>0.0 && length<3.402823e38)) { atomicStore(&valid[job],0u); }
}
@compute @workgroup_size(64)
fn resolve_hits(@builtin(global_invocation_id) id: vec3<u32>) {
    if (id.x>=rayCount) { return; }
    let job=id.x/(config.pageSize*config.pageSize);
    let hit=hits[id.x]; let ray=rays[id.x];
    var depth=select(1.0,0.0,config.reverseZ!=0u);
    if (hit.state==1u) {
        let layer=unpack_layered_coords(jobs[job].x).z;
        let clip=matrices[layer].viewProj*vec4<f32>(ray.origin+ray.direction*hit.distance,1.0);
        depth=clip.z/clip.w;
        if (!(depth>=0.0 && depth<=1.0)) { atomicStore(&valid[job],0u); }
    } else if (hit.state!=0u) { atomicStore(&valid[job],0u); }
    depths[id.x]=depth;
}
@compute @workgroup_size(64)
fn finish_pages(@builtin(global_invocation_id) id: vec3<u32>) {
    if (id.x>=rayCount/(config.pageSize*config.pageSize) || atomicLoad(&valid[id.x])==0u) { return; }
    let job=jobs[id.x]; let index=page_index(job.x);
    let current=atomicLoad(&vpt[index]);
    if (current==job.y) { atomicStore(&vpt[index],set_dirty(current,false)); }
}
