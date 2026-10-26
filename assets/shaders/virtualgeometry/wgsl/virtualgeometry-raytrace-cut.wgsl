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
// [0..2] the convert dispatch; [3] this buffer's cut stamp; [4 + 4 x instance
// + page % 4] the stamp of the cut that last emitted that instance's BLAS for
// that page (a kit geometry's pages are consecutive, at most four).
@group(0) @binding(11) var<storage,read_write> dispatch: array<atomic<u32>>;
// Invalid count deliberately exceeds the native capacity. The backend propagates
// InvalidScene for every ray; partial geometry must never appear fully lit.
@compute @workgroup_size(1)
fn prepare_cut() {
    let n=counters[HW_VISIBLE_CLUSTER_COUNT_INDEX];
    let invalid=counters[32]!=0u || counters[HIERARCHY_QUEUE_SIZE_INDEX]!=0u ||
        counters[SW_VISIBLE_CLUSTER_COUNT_INDEX]!=0u || n>config.capacity || n>arrayLength(&visible);
    atomicStore(&count,select(n,config.capacity+1u,invalid));
    atomicStore(&dispatch[0],(min(n,config.capacity)+63u)/64u); atomicStore(&dispatch[1],1u); atomicStore(&dispatch[2],1u);
    let stamp=atomicLoad(&dispatch[3])+1u;
    atomicStore(&dispatch[3],select(stamp,1u,stamp==0u));
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
    // Installed without a BLAS (budget full): an empty instance the backend
    // accepts as no caster; any other missing handle fails the scene.
    if (manifest.x==0xffffffffu && manifest.y==entry.pageIndex) { result.geometryHandle=0xffffffffu; output[i]=result; return; }
    // A whole-page BLAS (kPageBlasWholePage) covers every cluster of the
    // page: one instance per placement, from whichever of its clusters the
    // scene cut reaches first this frame; the rest are empty instances.
    let wholePage=(manifest.x&0x80000000u)!=0u;
    let blas=manifest.x&0x7fffffffu;
    if (blas==0u || manifest.y!=entry.pageIndex || hierarchy[entry._padding].meshPartIndex!=0xffffffffu) { invalid_cut(); return; }
    if (wholePage) {
        let slot=4u+entry.instanceIndex*4u+(entry.pageIndex&3u);
        if (slot>=arrayLength(&dispatch)) { invalid_cut(); return; }
        let stamp=atomicLoad(&dispatch[3]);
        if (atomicExchange(&dispatch[slot],stamp)==stamp) { result.geometryHandle=0xffffffffu; output[i]=result; return; }
    }
    let model=instances[entry.instanceIndex].modelMatrix;
    for (var col=0u;col<4u;col++) { for (var row=0u;row<3u;row++) { result.transform[col*3u+row]=model[col][row]; } }
    result.geometryHandle=blas; result.mask=255u; output[i]=result;
    var lo=vec3<f32>(3.402823e38); var hi=-lo;
    let first=select(cluster,page.clusterOffset,wholePage);
    let last=select(cluster+1u,min(page.clusterOffset+page.clusterCount,arrayLength(&clusters)),wholePage);
    for (var k=first;k<last;k++) {
        let c=clusters[k];
        lo=min(lo,vec3<f32>(c.localMinX,c.localMinY,c.localMinZ)); hi=max(hi,vec3<f32>(c.localMaxX,c.localMaxY,c.localMaxZ));
    }
    var worldLo=vec3<f32>(3.402823e38); var worldHi=-worldLo;
    for (var corner=0u;corner<8u;corner++) {
        let p=select(lo,hi,vec3<bool>((corner&1u)!=0u,(corner&2u)!=0u,(corner&4u)!=0u));
        let w=(model*vec4<f32>(p,1.0)).xyz; worldLo=min(worldLo,w); worldHi=max(worldHi,w);
    }
    bounds[i]=Bounds(vec4<f32>(worldLo,0.0),vec4<f32>(worldHi,0.0));
}
