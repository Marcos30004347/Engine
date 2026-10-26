// ============================================================================
// Virtual Geometry — visibility merge (virtualgeometry-visibility-merge-cs)
//
// Folds every rasterizer's output for a pixel into one visibility key: the
// hardware path's depth attachment and payload attachment, and the software
// rasterizer's depth/payload buffers, one pair per occlusion phase. Nearest
// wins; equal depths take the larger payload, so the result does not depend
// on which rasterizer or phase produced a surface.
// ============================================================================

#include "virtualgeometry-visibility.wgsl"

struct VisibilityMergeUniforms {
    viewport       : vec2<u32>,
    // Software phases to merge: 0, 1 or 2.
    softwarePhases : u32,
    nativePhaseMask : u32,
}

@group(0) @binding(0) var<uniform> uniforms : VisibilityMergeUniforms;
@group(0) @binding(1) var hardwareDepthTexture   : texture_depth_2d;
@group(0) @binding(2) var hardwarePayloadTexture : texture_2d<u32>;
@group(0) @binding(3) var<storage, read> softwareDepthA   : array<u32>;
@group(0) @binding(4) var<storage, read> softwarePayloadA : array<u32>;
@group(0) @binding(5) var<storage, read> softwareDepthB   : array<u32>;
@group(0) @binding(6) var<storage, read> softwarePayloadB : array<u32>;
@group(0) @binding(7) var<storage, read_write> visibilityKeys : array<VisibilityKey>;
@group(0) @binding(8) var hardwareInstanceTexture : texture_2d<u32>;
@group(0) @binding(9) var<storage, read> softwareInstanceA : array<u32>;
@group(0) @binding(10) var<storage, read> softwareInstanceB : array<u32>;

@compute @workgroup_size(8, 8, 1)
fn mergeVisibility(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let pixel = globalId.xy;
    if (any(pixel >= uniforms.viewport)) {
        return;
    }
    let index = pixel.y * uniforms.viewport.x + pixel.x;
    let pixelI = vec2<i32>(pixel);

    var key = VisibilityKey(
        visibilityDepthBits(textureLoad(hardwareDepthTexture, pixelI, 0)),
        textureLoad(hardwarePayloadTexture, pixelI, 0).x,
        textureLoad(hardwareInstanceTexture, pixelI, 0).x);
    if (uniforms.softwarePhases >= 1u && index < arrayLength(&softwareDepthA)) {
        let owner = select(softwareInstanceA[index], 0xffffffffu, (uniforms.nativePhaseMask & 1u) != 0u);
        let candidate = VisibilityKey(softwareDepthA[index], softwarePayloadA[index], owner);
        if (visibilityKeyWins(candidate, key)) {
            key = candidate;
        }
    }
    if (uniforms.softwarePhases >= 2u && index < arrayLength(&softwareDepthB)) {
        let owner = select(softwareInstanceB[index], 0xffffffffu, (uniforms.nativePhaseMask & 2u) != 0u);
        let candidate = VisibilityKey(softwareDepthB[index], softwarePayloadB[index], owner);
        if (visibilityKeyWins(candidate, key)) {
            key = candidate;
        }
    }
    if (visibilityKeyIsEmpty(key)) {
        key = visibilityKeyEmpty();
    }
    visibilityKeys[index] = key;
}
