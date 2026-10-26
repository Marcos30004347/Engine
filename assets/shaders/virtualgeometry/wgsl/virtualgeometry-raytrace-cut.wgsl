#include "virtualgeometrydata.wgsl"
struct CutUniforms { capacity: u32, p0: u32, p1: u32, p2: u32, }
struct RTInstance { transform: array<f32,12>, geometryHandle: u32, mask: u32, }
struct Bounds { minimum: vec4<f32>, maximum: vec4<f32>, }
@group(0) @binding(0) var<uniform> config: CutUniforms;
@group(0) @binding(1) var<storage,read> visible: array<VisibleClusterInfo>;
@group(0) @binding(2) var<storage,read> counters: array<u32>;
@group(0) @binding(3) var<storage,read> pages: array<PageTableEntry>;
@group(0) @binding(4) var<storage,read> handles: array<vec2<u32>>;
@group(0) @binding(5) var<storage,read> instances: array<InstanceData>;
@group(0) @binding(6) var<storage,read> hierarchy: array<VirtualMeshHierarchy>;
@group(0) @binding(7) var<storage,read> clusters: array<DecodedClusterCullData>;
@group(0) @binding(8) var<storage,read_write> output: array<RTInstance>;
@group(0) @binding(9) var<storage,read_write> count: atomic<u32>;
@group(0) @binding(10) var<storage,read_write> bounds: array<Bounds>;
@group(0) @binding(11) var<storage,read_write> dispatch: array<u32>;
// Invalid count deliberately exceeds the native capacity. The backend propagates
// InvalidScene for every ray; partial geometry must never appear fully lit.
@compute @workgroup_size(1)
fn prepare_cut() {
    let n=counters[HW_VISIBLE_CLUSTER_COUNT_INDEX];
    let invalid=counters[32]!=0u || counters[HIERARCHY_QUEUE_SIZE_INDEX]!=0u ||
        counters[SW_VISIBLE_CLUSTER_COUNT_INDEX]!=0u || n>config.capacity || n>arrayLength(&visible);
    atomicStore(&count,select(n,config.capacity+1u,invalid));
    dispatch[0]=(min(n,config.capacity)+63u)/64u; dispatch[1]=1u; dispatch[2]=1u;
}
fn invalid_cut() { atomicMax(&count,config.capacity+1u); }
@compute @workgroup_size(64)
fn convert_cut(@builtin(global_invocation_id) id: vec3<u32>) {
    let i=id.x;
    if (i>=min(counters[HW_VISIBLE_CLUSTER_COUNT_INDEX],config.capacity)) { return; }
    // Initialize even invalid entries: native validation may inspect the bounded prefix.
    var result: RTInstance; result.mask=0u; result.geometryHandle=0u; output[i]=result;
    bounds[i]=Bounds(vec4<f32>(0.0),vec4<f32>(0.0));
    if (i>=arrayLength(&visible)) { invalid_cut(); return; }
    let entry=visible[i];
    if (entry.pageIndex>=arrayLength(&pages) || entry.instanceIndex>=arrayLength(&instances) || entry._padding>=arrayLength(&hierarchy)) { invalid_cut(); return; }
    let page=pages[entry.pageIndex];
    if (page.isInstalled==0u || entry.pageLocalClusterIndex>=page.clusterCount) { invalid_cut(); return; }
    let cluster=page.clusterOffset+entry.pageLocalClusterIndex;
    if (cluster<page.clusterOffset || cluster>=arrayLength(&handles) || cluster>=arrayLength(&clusters)) { invalid_cut(); return; }
    let manifest=handles[cluster];
    if (manifest.x==0u || manifest.y!=entry.pageIndex || hierarchy[entry._padding].meshPartIndex!=0xffffffffu) { invalid_cut(); return; }
    let model=instances[entry.instanceIndex].modelMatrix;
    for (var col=0u;col<4u;col++) { for (var row=0u;row<3u;row++) { result.transform[col*3u+row]=model[col][row]; } }
    result.geometryHandle=manifest.x; result.mask=255u; output[i]=result;
    let c=clusters[cluster];
    let lo=vec3<f32>(c.localMinX,c.localMinY,c.localMinZ); let hi=vec3<f32>(c.localMaxX,c.localMaxY,c.localMaxZ);
    var worldLo=vec3<f32>(3.402823e38); var worldHi=-worldLo;
    for (var corner=0u;corner<8u;corner++) {
        let p=select(lo,hi,vec3<bool>((corner&1u)!=0u,(corner&2u)!=0u,(corner&4u)!=0u));
        let w=(model*vec4<f32>(p,1.0)).xyz; worldLo=min(worldLo,w); worldHi=max(worldHi,w);
    }
    bounds[i]=Bounds(vec4<f32>(worldLo,0.0),vec4<f32>(worldHi,0.0));
}
