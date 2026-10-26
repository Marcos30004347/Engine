#include "sparse_voxel_types.wgsl"
#include "sparse_voxel_projection.wgsl"

@group(0) @binding(0) var<uniform> uniforms : SparseVoxelRasterUniforms;
@group(0) @binding(1) var<storage, read> instances : array<SparseVoxelInstance>;
@group(0) @binding(2) var<storage, read> materials : array<SparseVoxelMaterial>;
@group(0) @binding(3) var<storage, read> resolvedSurfaces : array<ResolvedVoxelSurface>;

struct SparseVoxelMaterialOverride {
    roughnessMetallic : vec4<f32>,
    normalFilter : vec4<f32>,
    distantFilter : vec4<f32>,
    distantFilterDetail : vec4<f32>,
    // x enabled, y colour variation, z height-normal strength, w maximum
    // inward recess in world metres.
    microDetail : vec4<f32>,
    // x finest world-space voxel size, y hard cavity-shadow strength.
    microGrid : vec4<f32>,
}

@group(0) @binding(8) var<uniform> materialOverride : SparseVoxelMaterialOverride;
@group(0) @binding(9) var<storage, read> microMaterials : array<SparseVoxelMicroMaterial>;

#include "sparse_voxel_micro_geometry.wgsl"

struct VoxelGBufferFragmentOutput {
    @builtin(frag_depth) depth : f32,
    @location(0) instanceId : u32,
    @location(1) voxelId : u32,
    @location(2) materialId : u32,
    @location(3) materialUv : vec2<f32>,
    @location(4) geometryNormal : vec4<f32>,
    @location(5) motionVector : vec2<f32>,
    @location(6) temporalId : u32,
}

fn cameraRayDirection(pixel : vec2<u32>) -> vec3<f32> {
    let cameraDirection = vec3<f32>(
        (f32(pixel.x) + 0.5 - uniforms.focalPrincipal.z) / uniforms.focalPrincipal.x,
        (f32(pixel.y) + 0.5 - uniforms.focalPrincipal.w) / uniforms.focalPrincipal.y,
        1.0);
    return normalize(transformVector(uniforms.cameraToWorld, cameraDirection));
}

fn localFaceNormal(packedFace : u32) -> vec3<f32> {
    let axis = packedFace & 3u;
    let sign = select(-1.0, 1.0, (packedFace & 4u) != 0u);
    var normal = vec3<f32>(0.0);
    normal[axis] = sign;
    return normal;
}

fn transformNormal(worldToModel : mat4x4<f32>, localNormal : vec3<f32>) -> vec3<f32> {
    let transformed = vec3<f32>(
        dot(worldToModel[0].xyz, localNormal),
        dot(worldToModel[1].xyz, localNormal),
        dot(worldToModel[2].xyz, localNormal));
    return normalize(transformed);
}

// Two-colour materials use the same face mode to gate both shading and the
// displaced depth/normal surface. A clear bit means bottom colour only on that
// category, so leaving parallax enabled here would make geometry disagree with
// the material pass.
fn materialBlendsOnFace(surface : ResolvedVoxelSurface) -> bool {
    let topOrBottomFace = (surface.packedLocalFace & 3u) == 1u;
    if ((surface.hitFlags & SPARSE_VOXEL_SURFACE_PACKED_MATERIAL) != 0u) {
        let packed = surface.reserved1;
        if ((packed & SPARSE_VOXEL_PACKED_SECONDARY_ENABLED) == 0u) {
            return true;
        }
        let mode = select(
            SPARSE_VOXEL_PACKED_BLEND_SIDE_FACES,
            SPARSE_VOXEL_PACKED_BLEND_TOP_BOTTOM_FACES,
            topOrBottomFace);
        return (packed & mode) != 0u;
    }
    if ((surface.hitFlags & SPARSE_VOXEL_SURFACE_NO_MATERIAL) == 0u &&
        surface.voxelIndex < arrayLength(&materials)) {
        let material = materials[surface.voxelIndex];
        if ((material.reserved0 >> 24u) == 0u) {
            return true;
        }
        let mode = select(
            SPARSE_VOXEL_MATERIAL_BLEND_SIDE_FACES,
            SPARSE_VOXEL_MATERIAL_BLEND_TOP_BOTTOM_FACES,
            topOrBottomFace);
        return (material.reserved1 & mode) != 0u;
    }
    return true;
}

fn microVoxelHash(coordinate : vec3<i32>, face : u32) -> u32 {
    var value = bitcast<u32>(coordinate.x) * 0x8da6b343u;
    value ^= bitcast<u32>(coordinate.y) * 0xd8163841u;
    value ^= bitcast<u32>(coordinate.z) * 0xcb1ab31fu;
    value ^= face * 0x9e3779b9u;
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

fn microVoxelHeight(
    cell : vec2<i32>, plane : i32, face : u32) -> f32 {
    let hash = microVoxelHash(vec3<i32>(cell, plane), face);
    return f32(hash & 0xffffu) * (2.0 / 65535.0) - 1.0;
}

fn safeVoxelNormal(value : vec3<f32>, fallback : vec3<f32>) -> vec3<f32> {
    let length2 = dot(value, value);
    return select(
        fallback, value * inverseSqrt(max(length2, 1.0e-12)),
        length2 > 1.0e-12);
}

#include "sparse_voxel_terrain_normal.wgsl"

struct MicroVoxelGeometry {
    position : vec3<f32>,
    normal : vec3<f32>,
}

// Re-evaluate the same deterministic 2-by-3 height cell used by the material
// pass. The selected horizontal micro-face is moved inward along the camera
// ray, so hardware depth, motion and geometry normals describe the effect too.
// No virtual side-wall triangles are emitted; their depth discontinuity is
// nevertheless visible to HZB, GTAO and screen-space traces.
fn applyMicroVoxelGeometry(
    surface : ResolvedVoxelSurface, worldPosition : vec3<f32>,
    rayDirection : vec3<f32>, faceNormal : vec3<f32>) -> MicroVoxelGeometry {
    let enabled = materialOverride.microDetail.x > 0.5;
    let baseVoxelSize = max(materialOverride.microGrid.x, 0.0);
    let maximumRecess = max(materialOverride.microDetail.w, 0.0);
    if (!enabled || baseVoxelSize <= 1.0e-6 || maximumRecess <= 1.0e-6) {
        return MicroVoxelGeometry(worldPosition, faceNormal);
    }

    let microWorldSize = baseVoxelSize / 3.0;
    let projectedMicroPixels = microWorldSize *
        min(abs(uniforms.focalPrincipal.x), abs(uniforms.focalPrincipal.y)) /
        max(surface.surfaceT, 1.0e-4);
    let detailFade = sparseVoxelMicroDetailFade(projectedMicroPixels);
    if (detailFade <= 0.0) {
        return MicroVoxelGeometry(worldPosition, faceNormal);
    }

    let face = surface.packedLocalFace & 7u;
    let axis = face & 3u;
    var tangentU = vec3<f32>(1.0, 0.0, 0.0);
    var tangentV = vec3<f32>(0.0, 1.0, 0.0);
    if (axis == 0u) {
        tangentU = vec3<f32>(0.0, 0.0, 1.0);
        tangentV = vec3<f32>(0.0, 1.0, 0.0);
    } else if (axis == 1u) {
        tangentU = vec3<f32>(1.0, 0.0, 0.0);
        tangentV = vec3<f32>(0.0, 0.0, 1.0);
    }

    let inside = worldPosition - faceNormal * baseVoxelSize * 0.01;
    let solidCell = vec3<i32>(floor(inside / baseVoxelSize));
    let plane = solidCell[axis];
    var microCoordinate = vec2<f32>(
        dot(worldPosition, tangentU) / baseVoxelSize * 2.0,
        dot(worldPosition, tangentV) / baseVoxelSize * 3.0);
    var microCell = vec2<i32>(floor(microCoordinate));
    let preliminaryHeight = microVoxelHeight(microCell, plane, face);

    let towardCamera = -rayDirection;
    let grazing = max(abs(dot(towardCamera, faceNormal)), 0.20);
    let parallaxDirection = vec2<f32>(
        dot(towardCamera, tangentU), dot(towardCamera, tangentV)) / grazing;
    let preliminaryRecess = (preliminaryHeight * 0.5 + 0.5) *
        maximumRecess * detailFade;
    let parallaxCells = clamp(
        parallaxDirection * preliminaryRecess / baseVoxelSize *
            vec2<f32>(2.0, 3.0),
        vec2<f32>(-0.40), vec2<f32>(0.40));
    microCoordinate -= parallaxCells;
    microCell = vec2<i32>(floor(microCoordinate));

    let height = microVoxelHeight(microCell, plane, face);
    let heightLeft = microVoxelHeight(
        microCell + vec2<i32>(-1, 0), plane, face);
    let heightRight = microVoxelHeight(
        microCell + vec2<i32>(1, 0), plane, face);
    let heightDown = microVoxelHeight(
        microCell + vec2<i32>(0, -1), plane, face);
    let heightUp = microVoxelHeight(
        microCell + vec2<i32>(0, 1), plane, face);
    let recessLeft = (heightLeft * 0.5 + 0.5) * maximumRecess;
    let recessRight = (heightRight * 0.5 + 0.5) * maximumRecess;
    let recessDown = (heightDown * 0.5 + 0.5) * maximumRecess;
    let recessUp = (heightUp * 0.5 + 0.5) * maximumRecess;
    let gradient = vec2<f32>(
        (recessRight - recessLeft) / max(baseVoxelSize, 1.0e-6),
        (recessUp - recessDown) /
            max(baseVoxelSize * (2.0 / 3.0), 1.0e-6));
    let normalStrength = clamp(
        materialOverride.microDetail.z, 0.0, 1.0) * detailFade;
    let detailedNormal = safeVoxelNormal(
        faceNormal - tangentU * gradient.x * normalStrength -
            tangentV * gradient.y * normalStrength,
        faceNormal);

    // An inward plane hit lies farther along a front-facing camera ray. Clamp
    // the denominator exactly like the parallax lookup so grazing pixels never
    // produce an unbounded depth offset.
    let recess = (height * 0.5 + 0.5) * maximumRecess * detailFade;
    let rayFaceCosine = min(dot(rayDirection, faceNormal), -0.20);
    let displacedDistance = surface.surfaceT - recess / rayFaceCosine;
    let displacedPosition = uniforms.cameraWorldPositionNear.xyz +
        rayDirection * displacedDistance;
    return MicroVoxelGeometry(displacedPosition, detailedNormal);
}

fn clipToUv(clip : vec4<f32>) -> vec2<f32> {
    let ndc = clip.xy / clip.w;
    // Sparse raster targets use top-left image coordinates. The projection
    // matrix itself is not Vulkan-Y-flipped, so clip-space +Y must map toward
    // the top of the image, consistently with LOD, culling and screen traces.
    return vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

@vertex
fn writeVoxelGBufferVertex(@builtin(vertex_index) vertexIndex : u32) -> @builtin(position) vec4<f32> {
    let corners = array<vec2<f32>, 6>(
        vec2<f32>(-1.0, -1.0), vec2<f32>(-1.0, 1.0), vec2<f32>(1.0, -1.0),
        vec2<f32>(1.0, -1.0), vec2<f32>(-1.0, 1.0), vec2<f32>(1.0, 1.0));
    return vec4<f32>(corners[vertexIndex], 0.0, 1.0);
}

@fragment
fn writeVoxelGBuffer(@builtin(position) fragmentPosition : vec4<f32>) -> VoxelGBufferFragmentOutput {
    let pixel = vec2<u32>(fragmentPosition.xy);
    let pixelIndex = pixel.y * uniforms.viewportTiles.x + pixel.x;
    let surface = resolvedSurfaces[pixelIndex];
    var output : VoxelGBufferFragmentOutput;
    if ((surface.hitFlags & SPARSE_VOXEL_SURFACE_HIT) == 0u) {
        output.depth = uniforms.gbufferClearDepth;
        output.instanceId = 0u;
        output.voxelId = 0u;
        output.materialId = 0u;
        output.materialUv = vec2<f32>(0.0);
        output.geometryNormal = vec4<f32>(0.0);
        output.motionVector = vec2<f32>(0.0);
        output.temporalId = 0u;
        return output;
    }

    let instance = instances[surface.instanceIndex];
    // Two kinds of resolver have no voxel index to key the material array with,
    // and neither may index it: occupancy-only representations, which carry no
    // appearance at all, and representations that carry appearance inline. Both
    // set `voxelIndex` invalid, so indexing anyway lands on whatever the bounds
    // clamp produces.
    let packedMaterial =
        (surface.hitFlags & SPARSE_VOXEL_SURFACE_PACKED_MATERIAL) != 0u;
    let hasMaterial =
        (surface.hitFlags & SPARSE_VOXEL_SURFACE_NO_MATERIAL) == 0u && !packedMaterial;
    var materialId = 0u;
    if (hasMaterial) {
        materialId = materials[surface.voxelIndex].materialId;
    }
    let worldOrigin = uniforms.cameraWorldPositionNear.xyz;
    let rayDirection = cameraRayDirection(pixel);
    let baseWorldPosition = worldOrigin + rayDirection * surface.surfaceT;
    let worldNormal = sparseVoxelSolidTerrainFaceNormal(
        surface,
        transformNormal(
            instance.worldToModel, localFaceNormal(surface.packedLocalFace)),
        rayDirection);
    let microEnabled = materialOverride.microDetail.x > 0.5 &&
        materialBlendsOnFace(surface) &&
        (surface.reserved1 & SPARSE_VOXEL_EXACT_MICRO_MATERIAL_FLAG) == 0u &&
        !sparseVoxelHasTerrainSurfaceContext(surface);
    let microHit = traceSparseVoxelMicroSurface(
        baseWorldPosition, rayDirection, worldNormal, surface.surfaceT,
        surface.packedLocalFace, uniforms.focalPrincipal,
        select(0.0, max(materialOverride.microGrid.x, 0.0), microEnabled),
        max(materialOverride.microDetail.w, 0.0));
    let worldPosition = select(
        baseWorldPosition, microHit.position, microHit.valid);
    var geometryNormal = sparseVoxelTerrainMacroNormal(
        pixel, surface, worldNormal);
    if (microHit.valid && microHit.isWall) {
        let wallNormalStrength = clamp(
            materialOverride.microDetail.z, 0.0, 1.0) *
            microHit.detailFade;
        geometryNormal = safeVoxelNormal(
            mix(worldNormal, microHit.normal, wallNormalStrength),
            worldNormal);
    }
    let currentClip = uniforms.worldToClip * vec4<f32>(worldPosition, 1.0);

    var motion = vec2<f32>(0.0);
    if ((uniforms.gbufferFlags & SPARSE_VOXEL_GBUFFER_HAS_PREVIOUS_FRAME) != 0u && abs(currentClip.w) > 1.0e-6) {
        let localPosition = transformPoint(instance.worldToModel, worldPosition);
        let previousWorldPosition = transformPoint(instance.previousModelToWorld, localPosition);
        let previousClip = uniforms.previousWorldToClip * vec4<f32>(previousWorldPosition, 1.0);
        if (abs(previousClip.w) > 1.0e-6) {
            motion = clipToUv(currentClip) - clipToUv(previousClip);
        }
    }

    output.depth = currentClip.z / currentClip.w;
    output.instanceId = surface.instanceIndex;
    // Temporal reuse keys on this, together with instanceId, and accepts a
    // reprojected sample only on exact equality. It is a surface *identity*, not
    // necessarily an index: a resolver carrying appearance inline still has to
    // put something per-voxel here, and the brick resolver does.
    output.voxelId = surface.voxelIndex;
    output.materialId = materialId;
    output.materialUv = vec2<f32>(0.0);
    // Generated terrain is literal solid occupancy.  Every resolved face is a
    // real receiver and therefore participates in the same visibility path;
    // zero marked a receiver as analytic and produced unshadowed page-shaped
    // voids whenever a side face became visible during refinement.
    output.geometryNormal = vec4<f32>(geometryNormal, 1.0);
    output.motionVector = motion;
    // A hierarchy node is a representation choice, not a surface identity, so
    // the general raster paths retain their LOD-stable face class. Bricks are
    // different: their packed identity is the exact brick cell and remains
    // stable while the camera moves. Publish it (with an explicit exact-id
    // marker) so lighting history cannot alias every other voxel with the same
    // face normal. The light field separately validates the face sign.
    output.temporalId = select(
        (surface.packedLocalFace & 7u) + 1u,
        SPARSE_VOXEL_TEMPORAL_ID_EXACT |
            ((surface.voxelIndex + 1u) & ~SPARSE_VOXEL_TEMPORAL_ID_EXACT),
        packedMaterial);
    return output;
}
