// ============================================================================
// Virtual Geometry — visibility key (virtualgeometry-visibility.wgsl)
//
// One 64-bit value per pixel, Nanite's layout: the reverse-Z device depth in
// the high word and packVisibilityPayload(globalCluster, triangle) in the low
// word, so nearest-wins is an unsigned maximum of the whole key. Both
// rasterizers produce this value. The hardware path produces it through its
// depth test - depth attachment plus payload attachment - and the software
// path through two atomic passes, because MoltenVK reports no 64-bit buffer
// atomics to WGSL; the merge pass folds every source into one key buffer that
// the resolve pass turns into the G-buffer. A key whose depth word is zero is
// empty: reverse-Z puts the far plane at zero and nothing lands exactly there.
// ============================================================================

struct VisibilityKey {
    depthBits : u32,
    payload   : u32,
}

// Non-negative IEEE floats order like their bit patterns, so device depth in
// [0, 1] compares correctly as u32 under atomicMax.
fn visibilityDepthBits(deviceDepth : f32) -> u32 {
    return bitcast<u32>(max(deviceDepth, 0.0));
}

fn visibilityDepthFromBits(bits : u32) -> f32 {
    return bitcast<f32>(bits);
}

fn visibilityKeyEmpty() -> VisibilityKey {
    return VisibilityKey(0u, 0u);
}

fn visibilityKeyIsEmpty(key : VisibilityKey) -> bool {
    return key.depthBits == 0u;
}

// Strict 64-bit greater-than on (depthBits, payload).
fn visibilityKeyWins(candidate : VisibilityKey, current : VisibilityKey) -> bool {
    return candidate.depthBits > current.depthBits ||
        (candidate.depthBits == current.depthBits && candidate.payload > current.payload);
}

// Screen-space triangle setup shared by the software rasterizer and the
// resolve pass, so a pixel resolved from a key lands on exactly the triangle
// that produced it.
//
// Clip convention: the hardware vertex shader negates clip Y under
// USE_REVERSE_Z; the viewport maps NDC (-1, -1) to the top-left pixel and
// device depth is z / w.
fn visibilityClipToScreen(clip : vec4<f32>, viewport : vec2<f32>) -> vec3<f32> {
    let ndc = clip.xyz / clip.w;
#ifdef USE_REVERSE_Z
    let y = -ndc.y;
#else
    let y = ndc.y;
#endif
    return vec3<f32>(
        (ndc.x * 0.5 + 0.5) * viewport.x,
        (y * 0.5 + 0.5) * viewport.y,
        ndc.z);
}

// Twice the signed area of (a, b, p); positive when p lies to the left of the
// directed edge a -> b in y-down screen space.
fn visibilityEdge(a : vec2<f32>, b : vec2<f32>, p : vec2<f32>) -> f32 {
    return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);
}

// Top-left fill rule for a directed edge of a positive-area triangle: a left
// edge runs upward, a top edge runs leftward. Pixels exactly on such an edge
// belong to the triangle; pixels on any other edge do not.
fn visibilityEdgeIsTopLeft(a : vec2<f32>, b : vec2<f32>) -> bool {
    let d = b - a;
    return d.y < 0.0 || (d.y == 0.0 && d.x < 0.0);
}
