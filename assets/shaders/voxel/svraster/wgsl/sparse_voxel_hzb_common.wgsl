// Shared HZB texel addressing. Deliberately free of any binding or uniform
// dependency so the brick pass, the LOD pass and TLAS culling can all include
// it without dragging in each other's declarations.
// Mapping a screen UV onto a HZB mip's texel grid.
//
// A mip texel covers exactly 2^level base pixels, so the texel a UV falls in is
// `uv * baseDimensions / 2^level` - **not** `uv * storedMipDimensions`. The two
// differ whenever the base dimensions are not a multiple of 2^level: the stored
// mip rounds its size down, and the remainder rows and columns are never
// reduced into it at all. At 1280x720 the two coarsest levels each drop the
// bottom 16 rows; at a render scale of 0.5 the coarsest drops 40 of 360.
//
// Using the stored dimensions stretches the mip across the whole screen, so
// every texel is read at a growing offset from the geometry it describes and
// the far edge is read worst. That is a brick tested against an occluder
// belonging to a different part of the screen, which is how geometry that is
// really visible gets culled - near the right and bottom edges, intermittently,
// as the camera moves bricks across the misalignment.
fn sparseVoxelHZBTexelsPerUV(baseDimensions : vec2<f32>, level : i32) -> vec2<f32> {
    return baseDimensions / exp2(f32(max(level, 0)));
}

// The fraction of the screen a mip actually represents. Past it the mip holds
// nothing for the pixel, and the only safe answer is "visible".
fn sparseVoxelHZBCoveredFraction(
    baseDimensions : vec2<f32>, mipDimensions : vec2<f32>, level : i32
) -> vec2<f32> {
    let covered = mipDimensions * exp2(f32(max(level, 0)));
    return clamp(
        covered / max(baseDimensions, vec2<f32>(1.0)),
        vec2<f32>(0.0), vec2<f32>(1.0));
}
