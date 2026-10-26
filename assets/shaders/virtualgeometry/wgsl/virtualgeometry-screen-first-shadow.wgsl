// Matches the existing sparse-voxel screen tracer's ray and hit records.
struct SceneRay { originMinimumDistance: vec4<f32>, directionMaximumDistance: vec4<f32>,
    pixelRayLight: vec4<u32>, seedFlags: vec4<u32>, lightSamplePdfWeight: vec4<f32>, }
struct ScreenHit { hitPositionDistance: vec4<f32>, globalCell: vec3<i32>, status: u32, identity: vec4<u32>, }
struct NativeRay { origin: vec3<f32>, minDistance: f32, direction: vec3<f32>, maxDistance: f32,
    mask: u32, p0: u32, p1: u32, p2: u32, }
struct Config { capacity: u32, blocks: u32, p0: u32, p1: u32, }
@group(0) @binding(0) var<uniform> config: Config;
@group(0) @binding(1) var<storage,read> sceneRays: array<SceneRay>;
@group(0) @binding(2) var<storage,read> screenHits: array<ScreenHit>;
@group(0) @binding(3) var<storage,read> sourceState: array<u32>;
@group(0) @binding(4) var<storage,read_write> ranks: array<u32>;
@group(0) @binding(5) var<storage,read_write> blocks: array<u32>;
@group(0) @binding(6) var<storage,read_write> worldRays: array<NativeRay>;
@group(0) @binding(7) var<storage,read_write> worldCount: u32;
@group(0) @binding(8) var<storage,read> worldResults: array<u32>;
@group(0) @binding(9) var<storage,read_write> visibility: array<u32>;
@group(0) @binding(10) var<storage,read> activeIndices: array<u32>;
@group(0) @binding(11) var<storage,read_write> mergedHits: array<ScreenHit>;
fn source_count() -> u32 { return sourceState[select(0u,3u,(config.p0&1u)!=0u)]; }
fn canonical(i: u32) -> u32 {
    if ((config.p0&1u)!=0u) { return activeIndices[i]; }
    return i;
}

fn ray_active(i: u32) -> bool {
    return i<min(source_count(),config.capacity) && canonical(i)<config.capacity && (sceneRays[canonical(i)].seedFlags.y&3u)==3u;
}
fn screen_blocked(i: u32) -> bool {
    if ((config.p0&2u)==0u) { return false; }
    let index=canonical(i); let ray=sceneRays[index]; let hit=screenHits[index];
    return hit.status==2u && hit.identity.z==1u && hit.identity.x==ray.seedFlags.x &&
        hit.identity.y==ray.pixelRayLight.w && hit.identity.w==ray.seedFlags.z &&
        hit.hitPositionDistance.w>=max(ray.originMinimumDistance.w,0.0) &&
        hit.hitPositionDistance.w<=ray.directionMaximumDistance.w;
}
fn publish(i: u32, result: u32) {
    let index=canonical(i); visibility[index]=result;
    if ((config.p0&4u)==0u) { return; }
    if (screen_blocked(i) && result==1u) { mergedHits[index]=screenHits[index]; return; }
    let ray=sceneRays[index];
    var status=6u; // Unknown remains pending, never visible or occluded.
    var distance=-1.0; // Any-hit intentionally does not supply a hit position.
    if (result==0u) { status=3u; distance=ray.directionMaximumDistance.w; }
    if (result==1u) { status=2u; }
    mergedHits[index]=ScreenHit(vec4<f32>(0.0,0.0,0.0,distance),vec3<i32>(-2147483648),status,
        vec4<u32>(ray.seedFlags.x,ray.pixelRayLight.w,3u,ray.seedFlags.z));
}
var<workgroup> scan: array<u32,256>;
var<workgroup> malformed: atomic<u32>;
@compute @workgroup_size(256)
fn rank_unresolved(@builtin(global_invocation_id) id: vec3<u32>,@builtin(local_invocation_index) lane: u32) {
    if (lane==0u) { atomicStore(&malformed,0u); } workgroupBarrier();
    if (id.x<min(source_count(),config.capacity) && canonical(id.x)>=config.capacity) {
        atomicStore(&malformed,0x80000000u);
    }
    var unresolved=0u;
    if (ray_active(id.x)) { unresolved=select(1u,0u,screen_blocked(id.x)); }
    scan[lane]=unresolved; workgroupBarrier();
    for (var offset=1u;offset<256u;offset*=2u) {
        var v=0u; if (lane>=offset) { v=scan[lane-offset]; }
        workgroupBarrier(); scan[lane]+=v; workgroupBarrier();
    }
    if (id.x<config.capacity) { ranks[id.x]=scan[lane]-unresolved; }
    if (lane==255u) { blocks[id.x/256u]=scan[lane]|atomicLoad(&malformed); }
}
@compute @workgroup_size(1)
fn prefix_blocks() {
    var prefix=0u; var invalid=source_count()>config.capacity;
    for (var i=0u;i<config.blocks;i++) {
        let n=blocks[i]; invalid=invalid || (n&0x80000000u)!=0u;
        blocks[i]=prefix; prefix+=n&0x7fffffffu;
    }
    worldCount=select(prefix,config.capacity+1u,invalid);
}
@compute @workgroup_size(256)
fn compact_rays(@builtin(global_invocation_id) id: vec3<u32>) {
    if (id.x>=config.capacity) { return; }
    if ((config.p0&1u)==0u) { visibility[id.x]=2u; }
    else if (id.x<min(source_count(),config.capacity) && canonical(id.x)<config.capacity) {
        visibility[canonical(id.x)]=2u;
    }
    if (!ray_active(id.x)) { return; }
    publish(id.x,2u);
    if (worldCount>config.capacity) { return; }
    if (screen_blocked(id.x)) { publish(id.x,1u); return; }
    let destination=blocks[id.x/256u]+ranks[id.x]; let ray=sceneRays[canonical(id.x)];
    // Scene-ray distances are world units: the screen and Tree64 producers
    // normalize this direction too. Preserve zero as an invalid native ray.
    var direction=vec3<f32>(0.0);
    if (dot(ray.directionMaximumDistance.xyz,ray.directionMaximumDistance.xyz)>=1.0e-12) {
        direction=normalize(ray.directionMaximumDistance.xyz);
    }
    var origin=ray.originMinimumDistance.xyz;
    var distance=ray.directionMaximumDistance.w;
    let face=(ray.seedFlags.y>>4u)&7u;
    let bias=bitcast<f32>(config.p1);
    if (bias>0.0 && face>=1u && face<=6u && distance>0.0 && dot(direction,direction)>0.0) {
        let axis=(face-1u)/2u;
        let sign=select(-1.0,1.0,(face&1u)!=0u);
        // Cover reconstruction rounding as well as triangle t=0 hits. The
        // provider uses a tiny fraction of a voxel, not an entire skipped cell.
        var offset=vec3<f32>(0.0);
        // Reconstruction mixes all world coordinates through the view transform.
        // An elevated Y face at a large X/Z origin inherits their rounding
        // error too; using Y alone left the ray inside that face.
        let coordinateMagnitude=max(max(abs(origin.x),abs(origin.y)),abs(origin.z));
        offset[axis]=sign*max(bias,coordinateMagnitude*9.5367431640625e-7);
        origin+=offset;
        // Keep the sampled finite emitter endpoint fixed after moving origin.
        let segment=direction*distance-offset;
        distance=length(segment); direction=segment/max(distance,1.0e-20);
    }
    worldRays[destination]=NativeRay(origin,ray.originMinimumDistance.w,
        direction,distance,255u,0u,0u,0u);
}
@compute @workgroup_size(256)
fn merge_visibility(@builtin(global_invocation_id) id: vec3<u32>) {
    if (!ray_active(id.x) || worldCount>config.capacity || screen_blocked(id.x)) { return; }
    let source=blocks[id.x/256u]+ranks[id.x]; publish(id.x,worldResults[source]);
}
