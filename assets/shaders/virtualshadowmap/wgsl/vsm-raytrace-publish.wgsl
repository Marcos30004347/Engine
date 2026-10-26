#include "vsm-common.wgsl"
struct Config { pageResolution: u32, pageSize: u32, atlasResolution: u32, capacity: u32,
    reverseZ: u32, rayBudget: u32, p0: u32, p1: u32, }
@group(0) @binding(0) var<uniform> config: Config;
@group(0) @binding(7) var<storage,read> jobs: array<vec2<u32>>;
@group(0) @binding(8) var<storage,read> rayCount: u32;
@group(0) @binding(11) var<storage,read> depths: array<f32>;
@group(0) @binding(12) var<storage,read> valid: array<u32>;
struct Vertex { @builtin(position) position: vec4<f32>, @location(0) @interpolate(flat) job: u32, }
@vertex fn publish_vs(@builtin(vertex_index) vertex: u32,@builtin(instance_index) job: u32) -> Vertex {
    var out: Vertex; out.job=job; out.position=vec4<f32>(-2.0,-2.0,0.0,1.0);
    if (job>=rayCount/(config.pageSize*config.pageSize) || valid[job]==0u) { return out; }
    let entry=jobs[job].y;
    let origin=vec2<u32>(get_page_x(entry),get_page_y(entry))*config.pageSize;
    let corners=array<vec2<f32>,6>(vec2<f32>(0,0),vec2<f32>(1,0),vec2<f32>(0,1),vec2<f32>(0,1),vec2<f32>(1,0),vec2<f32>(1,1));
    let uv=(vec2<f32>(origin)+corners[vertex]*f32(config.pageSize))/f32(config.atlasResolution);
    out.position=vec4<f32>(uv*2.0-vec2<f32>(1.0),0.0,1.0); return out;
}
@fragment fn publish_fs(in: Vertex) -> @builtin(frag_depth) f32 {
    let entry=jobs[in.job].y;
    let origin=vec2<u32>(get_page_x(entry),get_page_y(entry))*config.pageSize;
    let pixel=vec2<u32>(in.position.xy)-origin;
    return depths[in.job*config.pageSize*config.pageSize+pixel.y*config.pageSize+pixel.x];
}
// GPU validation/readback entry point; the production publication pipeline does
// not bind these resources. Samples the real depth attachment after publication.
@group(0) @binding(14) var atlas: texture_depth_2d;
@group(0) @binding(15) var<storage,read_write> sampled: array<f32>;
@compute @workgroup_size(64)
fn sample_published_depth(@builtin(global_invocation_id) id: vec3<u32>) {
    if (id.x>=rayCount) { return; }
    let texels=config.pageSize*config.pageSize; let entry=jobs[id.x/texels].y;
    let origin=vec2<u32>(get_page_x(entry),get_page_y(entry))*config.pageSize;
    let pixel=id.x%texels;
    sampled[id.x]=textureLoad(atlas,vec2<i32>(origin+vec2<u32>(pixel%config.pageSize,pixel/config.pageSize)),0);
}
