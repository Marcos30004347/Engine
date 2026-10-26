// Shared by demand and resolve. The smaller singular value constrains both
// axes, unlike a max-axis footprint. Missing/discontinuous derivatives request
// the finest level; the caller still walks outward to establish coverage.
fn vsm_directional_neighbor(pixel: vec2<i32>, center: vec3<f32>, delta: vec2<i32>) -> vec3<f32> {
    let dims=textureDimensions(depthTexture);
    let p=pixel+delta;
    if (any(p<vec2<i32>(0)) || any(p>=vec2<i32>(dims))) { return center; }
    let depth=textureLoad(depthTexture,p,0);
    if (select(depth>=1.0,depth<=0.0,cameraState.reverseZ!=0u)) { return center; }
    return vsm_reconstruct_world_position_from_uv_depth(vsm_texel_center_uv(vec2<u32>(p),dims),depth,cameraState.inverseViewProjection);
}
fn vsm_directional_footprint_level(pixel: vec2<u32>, center: vec3<f32>, firstLayer: u32, radial: u32) -> u32 {
    let texelTarget=cameraState.targetShadowTexelPixels;
    if (texelTarget<=0.0) { return radial; }
    let left=center-vsm_directional_neighbor(vec2<i32>(pixel),center,vec2<i32>(-1,0));
    let right=vsm_directional_neighbor(vec2<i32>(pixel),center,vec2<i32>(1,0))-center;
    let up=center-vsm_directional_neighbor(vec2<i32>(pixel),center,vec2<i32>(0,-1));
    let down=vsm_directional_neighbor(vec2<i32>(pixel),center,vec2<i32>(0,1))-center;
    var dx=select(left,right,dot(right,right)<dot(left,left));
    var dy=select(up,down,dot(down,down)<dot(up,up));
    if (dot(left,left)==0.0) { dx=right; }
    if (dot(right,right)==0.0) { dx=left; }
    if (dot(up,up)==0.0) { dy=down; }
    if (dot(down,down)==0.0) { dy=up; }
    // Large jumps cannot establish a same-surface derivative.
    let limit=max(distance(center,cameraState.cameraPosition.xyz)*0.02,0.001);
    if (length(dx)>limit || length(dy)>limit) { return 0u; }
    let matrix=cascadeMatrices[firstLayer].viewProj;
    let scale=f32(uniforms.pageTableResolution*uniforms.physicalPageSize)*0.5;
    let a=(matrix*vec4<f32>(dx,0.0)).xy*scale;
    let b=(matrix*vec4<f32>(dy,0.0)).xy*scale;
    let trace=dot(a,a)+dot(b,b);
    let det=(a.x*b.y-a.y*b.x)*(a.x*b.y-a.y*b.x);
    let sigma=sqrt(max(2.0*det/max(trace+sqrt(max(trace*trace-4.0*det,0.0)),1e-20),0.0));
    return min(u32(max(floor(log2(max(sigma*texelTarget,1e-20))),0.0)),max(uniforms.cascadeCount,1u)-1u);
}
