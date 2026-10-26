// Reconstruct the continuous terrain slope underneath exact staircase
// micro-geometry. The staircase remains the depth/visibility surface, while
// lighting and secondary-ray origins follow the macro heightfield instead of
// treating every one-microcell riser as a black cliff.
//
// Include after sparse_voxel_micro_geometry.wgsl, transformNormal(), and
// safeVoxelNormal(). The including shader supplies `uniforms`, `instances`,
// and `resolvedSurfaces` through the standard sparse-voxel G-buffer contract.

struct SparseVoxelTerrainNormalSample {
    position : vec3<f32>,
    distance : f32,
    valid : bool,
}

fn sparseVoxelHasTerrainSurfaceContext(
    surface : ResolvedVoxelSurface) -> bool {
    if ((surface.hitFlags & SPARSE_VOXEL_SURFACE_PACKED_MATERIAL) == 0u) {
        return false;
    }
    let context = sparseVoxelResolvedContext(surface);
    return context == SPARSE_VOXEL_MICRO_CONTEXT_FLAT_SURFACE ||
        (context >= SPARSE_VOXEL_MICRO_CONTEXT_WALL_NEGATIVE_X &&
         context <= SPARSE_VOXEL_MICRO_CONTEXT_WALL_POSITIVE_Z) ||
        (context & SPARSE_VOXEL_MICRO_CONTEXT_SURFACE_FLAG) != 0u;
}

fn sparseVoxelIsExactTerrainSurface(
    surface : ResolvedVoxelSurface) -> bool {
    // Exact micro payloads may be absent on terrain pages after distance
    // handoff or coarse-page cutoff. The context byte still identifies the
    // surface as terrain, and using it here prevents those pages from falling
    // back to literal cube/riser normals or legacy random micro detail.
    return sparseVoxelHasTerrainSurfaceContext(surface);
}

// Runtime-generated terrain is the literal closed occupancy emitted by the
// GPU page builder.  It has neither analytic nor exact-micro geometry: every
// resolved face belongs to one solid voxel and must keep that face normal.
fn sparseVoxelIsGeneratedSolidTerrain(
    surface : ResolvedVoxelSurface) -> bool {
    return sparseVoxelHasTerrainSurfaceContext(surface) &&
        (surface.hitFlags & SPARSE_VOXEL_SURFACE_ANALYTIC_TERRAIN) == 0u &&
        (surface.reserved1 &
            SPARSE_VOXEL_EXACT_MICRO_MATERIAL_FLAG) == 0u;
}

// The generated-terrain brick walk is an entry-surface DDA: when the camera
// starts inside a conservative brick bound it skips the occupied run at the
// eye and resumes only after reaching empty space. Consequently every
// published solid-terrain hit must oppose the primary ray. Boundary ties
// between independently published pages could retain the exit-face sign even
// though the selected distance was the neighbouring entry. Canonicalize that
// one bit here; unlike the removed macro normal this depends only on the hit
// and ray and cannot vary with screen neighbours or temporal jitter.
fn sparseVoxelSolidTerrainFaceNormal(
    surface : ResolvedVoxelSurface, normal : vec3<f32>,
    rayDirection : vec3<f32>) -> vec3<f32> {
    return select(
        normal, -normal,
        sparseVoxelIsGeneratedSolidTerrain(surface) &&
            dot(normal, rayDirection) > 0.0);
}

fn sparseVoxelIsGeneratedTerrainClosure(
    surface : ResolvedVoxelSurface) -> bool {
    if (!sparseVoxelIsGeneratedSolidTerrain(surface)) {
        return false;
    }
    let faceAxis = surface.packedLocalFace & 3u;
    let positiveY = faceAxis == 1u &&
        (surface.packedLocalFace & 4u) != 0u;
    return !positiveY;
}

fn sparseVoxelUsesTerrainMacroNormal(
    surface : ResolvedVoxelSurface) -> bool {
    // Screen-space reconstruction is valid only for the two explicitly
    // authored continuous-surface representations. Applying it to generated
    // occupancy made a face depend on neighbouring screen pixels and caused
    // grid/radial lines plus frame-to-frame normal and shadow motion.
    return (surface.hitFlags & SPARSE_VOXEL_SURFACE_ANALYTIC_TERRAIN) != 0u ||
        (surface.reserved1 & SPARSE_VOXEL_EXACT_MICRO_MATERIAL_FLAG) != 0u;
}

fn sparseVoxelTerrainWorldPosition(
    pixel : vec2<i32>, distance : f32) -> vec3<f32> {
    let cameraDirection = vec3<f32>(
        (f32(pixel.x) + 0.5 - uniforms.focalPrincipal.z) /
            max(uniforms.focalPrincipal.x, 1.0e-4),
        (f32(pixel.y) + 0.5 - uniforms.focalPrincipal.w) /
            max(uniforms.focalPrincipal.y, 1.0e-4),
        1.0);
    let worldDirection = normalize(
        (uniforms.cameraToWorld * vec4<f32>(cameraDirection, 0.0)).xyz);
    return uniforms.cameraWorldPositionNear.xyz + worldDirection * distance;
}

fn sparseVoxelTerrainNormalSample(
    coordinate : vec2<i32>, dimensions : vec2<u32>,
    centerInstance : u32) -> SparseVoxelTerrainNormalSample {
    if (any(coordinate < vec2<i32>(0)) ||
        any(coordinate >= vec2<i32>(dimensions))) {
        return SparseVoxelTerrainNormalSample(
            vec3<f32>(0.0), 0.0, false);
    }
    let index = u32(coordinate.y) * dimensions.x + u32(coordinate.x);
    if (index >= arrayLength(&resolvedSurfaces)) {
        return SparseVoxelTerrainNormalSample(
            vec3<f32>(0.0), 0.0, false);
    }
    let surface = resolvedSurfaces[index];
    if ((surface.hitFlags & SPARSE_VOXEL_SURFACE_HIT) == 0u ||
        surface.instanceIndex >= arrayLength(&instances) ||
        centerInstance >= arrayLength(&instances) ||
        !sparseVoxelIsExactTerrainSurface(surface)) {
        return SparseVoxelTerrainNormalSample(
            vec3<f32>(0.0), 0.0, false);
    }
    // Generated terrain pages are independent raster instances, but their
    // touching faces belong to one continuous height field. Requiring the
    // neighbour to have the same instance id makes every finite-difference
    // stencil stop at a page boundary. The centre then falls back to terrain
    // up (or to its literal closure-face normal), producing the dark page grid
    // and stair-step teeth visible with lighting enabled even though primary
    // depth has complete coverage.
    //
    // Do retain an orientation guard. It keeps unrelated terrain patches from
    // being joined merely because they overlap in screen space while allowing
    // translated world pages, whose model-space up vectors are identical, to
    // reconstruct one slope across their ownership boundary.
    let centerUp = transformNormal(
        instances[centerInstance].worldToModel, vec3<f32>(0.0, 1.0, 0.0));
    let sampleUp = transformNormal(
        instances[surface.instanceIndex].worldToModel,
        vec3<f32>(0.0, 1.0, 0.0));
    if (dot(centerUp, sampleUp) < 0.999) {
        return SparseVoxelTerrainNormalSample(
            vec3<f32>(0.0), 0.0, false);
    }
    return SparseVoxelTerrainNormalSample(
        sparseVoxelTerrainWorldPosition(coordinate, surface.surfaceT),
        surface.surfaceT, true);
}

// How much of the screen-space reconstruction this pixel is entitled to.
//
// The reconstruction answers one question: what should a pixel shade as when
// it covers several micro steps at once? There the staircase is pure aliasing,
// the pixel really does see both tread and riser, and the area-weighted macro
// slope is the honest shading normal.
//
// None of that holds once a single step spans many pixels. A near-field pixel
// sits entirely on one face whose exact normal is already known, and a
// four-pixel screen-space fit has nothing to add except within two pixels of
// the face border - where the taps land on the perpendicular face and tip the
// normal toward it. That tipped band is the bright rim traced along every
// edge, and the pixels where the taps fail outright are worse still: the
// fallback substitutes terrain-up on a vertical riser, which is a *sunlit*
// normal on a face the sun cannot reach, so the rim reads at the brightness of
// the lit ground rather than of the shadowed face it sits on. Pixel-to-pixel
// disagreement over which taps survive is the speckle in the same band.
//
// So gate on the projected size of one micro step and give the near field its
// exact face normal back. The horizon band - where the reconstruction was
// measured to matter - is far away, keeps a sub-pixel step, and is untouched.
fn sparseVoxelTerrainMacroWeight(surface : ResolvedVoxelSurface) -> f32 {
    if ((surface.hitFlags & SPARSE_VOXEL_SURFACE_ANALYTIC_TERRAIN) != 0u) {
        return 1.0;
    }
    let context = sparseVoxelResolvedContext(surface);
    let encodedClosure =
        context >= SPARSE_VOXEL_MICRO_CONTEXT_WALL_NEGATIVE_X &&
        context <= SPARSE_VOXEL_MICRO_CONTEXT_WALL_POSITIVE_Z;
    // Parent-voxel terrain does not need the legacy wall context to expose a
    // side: packedLocalFace is the authoritative raster face. Without this
    // test, close side faces carrying FlatSurface reached the projected-size
    // cutoff below and reverted to a literal vertical normal. Those are the
    // black contour lines that look like cracks between otherwise watertight
    // terrain cells.
    let faceAxis = surface.packedLocalFace & 3u;
    let parentClosure = faceAxis != 1u ||
        (faceAxis == 1u && (surface.packedLocalFace & 4u) == 0u);
    if (encodedClosure || parentClosure) {
        // These faces close the shell between adjacent heightfield columns;
        // they are not geological cliffs. Always shade them with the macro
        // heightfield normal so a resolved cell boundary cannot become a dark
        // dotted contour as its projected size changes.
        return 1.0;
    }
    let step = max(materialOverride.microGrid.x, 0.0);
    if (step <= 0.0) { return 1.0; }
    let focal = max(min(abs(uniforms.focalPrincipal.x),
                        abs(uniforms.focalPrincipal.y)), 1.0);
    let stepPixels = step * focal / max(abs(surface.surfaceT), 1.0e-4);
    // Full reconstruction while a step is at most about a pixel and a half,
    // none once the step resolves on its own, and smooth in between so the
    // handover is not itself an edge.
    return 1.0 - smoothstep(1.5, 6.0, stepPixels);
}

fn sparseVoxelTerrainMacroNormal(
    pixel : vec2<u32>, centerSurface : ResolvedVoxelSurface,
    fallback : vec3<f32>) -> vec3<f32> {
    if (!sparseVoxelIsExactTerrainSurface(centerSurface) ||
        !sparseVoxelUsesTerrainMacroNormal(centerSurface) ||
        centerSurface.instanceIndex >= arrayLength(&instances)) {
        return fallback;
    }
    let macroWeight = sparseVoxelTerrainMacroWeight(centerSurface);
    if (macroWeight <= 0.0) {
        return fallback;
    }

    // A staircase riser is visibility geometry, not the macro terrain normal.
    // If screen neighbours cannot reconstruct the slope (silhouette, disocclusion
    // or a newly exposed microcell), fall back to terrain-up instead of shading
    // that tiny riser as a full black cliff.
    let terrainUp = transformNormal(
        instances[centerSurface.instanceIndex].worldToModel,
        vec3<f32>(0.0, 1.0, 0.0));

    // Current generated terrain is a square parent-voxel height field. Its
    // X/Z faces only close the quantized height columns; they are not authored
    // cliff normals. A screen-space derivative taken on the one-pixel closure
    // itself is ill-conditioned and produces a thin tilted normal surrounded
    // by upward tread normals—the exact contour that lighting turns black.
    // Match the adjoining tread here. Authored analytic/exact-micro terrain is
    // identified separately and retains its reconstructed slope path.
    let faceAxis = centerSurface.packedLocalFace & 3u;
    // A height field has one authored exterior: its upward surface.  X/Z
    // walls and the negative-Y bottom only close independently streamed voxel
    // columns.  They may become visible during LOD replacement, but must not
    // introduce a downward/side lighting field that turns a whole page black.
    let generatedParentClosure = faceAxis != 1u ||
        (faceAxis == 1u &&
         (centerSurface.packedLocalFace & 4u) == 0u);
    let authoredSlope =
        (centerSurface.hitFlags &
            SPARSE_VOXEL_SURFACE_ANALYTIC_TERRAIN) != 0u ||
        (centerSurface.reserved1 &
            SPARSE_VOXEL_EXACT_MICRO_MATERIAL_FLAG) != 0u;
    if (generatedParentClosure && !authoredSlope) {
        return terrainUp;
    }

    let dimensions = max(uniforms.viewportTiles.xy, vec2<u32>(1u));
    let centerPixel = vec2<i32>(pixel);
    let centerPosition = sparseVoxelTerrainWorldPosition(
        centerPixel, centerSurface.surfaceT);

    // Two pixels average several tiny risers at distance. Fall back to the
    // immediate neighbour at silhouettes and viewport edges.
    var left = sparseVoxelTerrainNormalSample(
        centerPixel + vec2<i32>(-2, 0), dimensions,
        centerSurface.instanceIndex);
    var right = sparseVoxelTerrainNormalSample(
        centerPixel + vec2<i32>(2, 0), dimensions,
        centerSurface.instanceIndex);
    var up = sparseVoxelTerrainNormalSample(
        centerPixel + vec2<i32>(0, -2), dimensions,
        centerSurface.instanceIndex);
    var down = sparseVoxelTerrainNormalSample(
        centerPixel + vec2<i32>(0, 2), dimensions,
        centerSurface.instanceIndex);
    if (!left.valid) {
        left = sparseVoxelTerrainNormalSample(
            centerPixel + vec2<i32>(-1, 0), dimensions,
            centerSurface.instanceIndex);
    }
    if (!right.valid) {
        right = sparseVoxelTerrainNormalSample(
            centerPixel + vec2<i32>(1, 0), dimensions,
            centerSurface.instanceIndex);
    }
    if (!up.valid) {
        up = sparseVoxelTerrainNormalSample(
            centerPixel + vec2<i32>(0, -1), dimensions,
            centerSurface.instanceIndex);
    }
    if (!down.valid) {
        down = sparseVoxelTerrainNormalSample(
            centerPixel + vec2<i32>(0, 1), dimensions,
            centerSurface.instanceIndex);
    }

    // Do not bridge an actual foreground/background discontinuity belonging
    // to the same terrain instance. A normal micro-riser is far smaller than
    // one parent voxel and stays comfortably inside this bound.
    let focal = max(min(abs(uniforms.focalPrincipal.x),
                        abs(uniforms.focalPrincipal.y)), 1.0);
    let pixelFootprint = max(abs(centerSurface.surfaceT) / focal, 1.0e-5);
    // Scaled by incidence. On ground seen at a grazing angle - which is the
    // whole horizon band - the distance to the surface changes by far more
    // than a pixel footprint from one pixel to the next, and that is the
    // surface itself, not a silhouette. With a fixed bound every neighbour
    // near the horizon was rejected as a discontinuity, the reconstruction
    // fell back to the raw riser normal, and 21-31% of terrain pixels there
    // shaded as unlit vertical faces. The reciprocal is clamped so a true
    // silhouette is still rejected rather than bridged at any angle.
    let viewDirection = normalize(
        sparseVoxelTerrainWorldPosition(centerPixel, 1.0) -
        uniforms.cameraWorldPositionNear.xyz);
    let incidence = max(abs(dot(terrainUp, viewDirection)), 0.02);
    let depthLimit = max(
        pixelFootprint * 24.0 / incidence,
        max(materialOverride.microGrid.x, 0.0) * 2.0);
    left.valid = left.valid &&
        abs(left.distance - centerSurface.surfaceT) <= depthLimit;
    right.valid = right.valid &&
        abs(right.distance - centerSurface.surfaceT) <= depthLimit;
    up.valid = up.valid &&
        abs(up.distance - centerSurface.surfaceT) <= depthLimit;
    down.valid = down.valid &&
        abs(down.distance - centerSurface.surfaceT) <= depthLimit;

    var tangentX = vec3<f32>(0.0);
    var tangentY = vec3<f32>(0.0);
    var hasX = false;
    var hasY = false;
    if (left.valid && right.valid) {
        tangentX = right.position - left.position;
        hasX = true;
    } else if (right.valid) {
        tangentX = right.position - centerPosition;
        hasX = true;
    } else if (left.valid) {
        tangentX = centerPosition - left.position;
        hasX = true;
    }
    if (up.valid && down.valid) {
        tangentY = down.position - up.position;
        hasY = true;
    } else if (down.valid) {
        tangentY = down.position - centerPosition;
        hasY = true;
    } else if (up.valid) {
        tangentY = centerPosition - up.position;
        hasY = true;
    }
    // Every fallback below leans on terrain-up only as far as this pixel is
    // entitled to lean on the reconstruction at all. At the far end that is the
    // whole way, which is the sub-pixel-staircase case the fallback was written
    // for; near the camera it is not at all, which is what stops a silhouette
    // pixel on a shadowed riser from being handed a sunlit normal.
    if (!hasX || !hasY) {
        return safeVoxelNormal(mix(fallback, terrainUp, macroWeight), fallback);
    }

    let crossValue = cross(tangentX, tangentY);
    if (dot(crossValue, crossValue) <= 1.0e-14) {
        return safeVoxelNormal(mix(fallback, terrainUp, macroWeight), fallback);
    }
    var macroNormal = safeVoxelNormal(crossValue, fallback);
    // A fit that comes out perpendicular to terrain-up has no reliable sign:
    // the flip below is then decided by a difference far below the noise of a
    // four-pixel finite difference, and neighbouring pixels resolve it in
    // opposite directions - a 180 degree swing that shows up as speckle rather
    // than as a gradient. Trust the exact face normal there instead.
    let upAlignment = dot(macroNormal, terrainUp);
    if (upAlignment < 0.0) {
        macroNormal = -macroNormal;
    }
    let context = sparseVoxelResolvedContext(centerSurface);
    let encodedSlope = context & SPARSE_VOXEL_MICRO_TERRAIN_SLOPE_MASK;
    if ((context & SPARSE_VOXEL_MICRO_CONTEXT_SURFACE_FLAG) != 0u &&
        encodedSlope != 0u) {
        // Generation stores atan(|gradient|)/(pi/2) in the context. The
        // screen-space fit supplies direction, but a neighbour landing on a
        // closure step must not turn a gentle source-map slope into an almost
        // vertical normal. Reapply the authoritative heightmap magnitude.
        let normalizedAngle = f32(encodedSlope - 1u) /
            SPARSE_VOXEL_MICRO_TERRAIN_SLOPE_QUANTIZATION;
        let angle = normalizedAngle * 1.5707963267948966;
        let tangentValue = macroNormal -
            terrainUp * dot(macroNormal, terrainUp);
        let fallbackTangent = fallback -
            terrainUp * dot(fallback, terrainUp);
        let tangentDirection = safeVoxelNormal(
            tangentValue, safeVoxelNormal(fallbackTangent,
                vec3<f32>(1.0, 0.0, 0.0)));
        macroNormal = safeVoxelNormal(
            terrainUp * cos(angle) + tangentDirection * sin(angle),
            terrainUp);
    }
    let encodedClosure =
        context >= SPARSE_VOXEL_MICRO_CONTEXT_WALL_NEGATIVE_X &&
        context <= SPARSE_VOXEL_MICRO_CONTEXT_WALL_POSITIVE_Z;
    let parentClosure = generatedParentClosure;
    if (encodedClosure || parentClosure) {
        let wallAlignment = clamp(dot(macroNormal, terrainUp), 0.0, 1.0);
        let minimumWallAlignment = 0.8660254;
        if (wallAlignment < minimumWallAlignment) {
            let tangentValue = macroNormal - terrainUp * wallAlignment;
            let tangentDirection = safeVoxelNormal(
                tangentValue, vec3<f32>(1.0, 0.0, 0.0));
            macroNormal = safeVoxelNormal(
                terrainUp * minimumWallAlignment + tangentDirection * 0.5,
                terrainUp);
        }
    }
    let correctedAlignment = abs(dot(macroNormal, terrainUp));
    let weight = macroWeight *
        smoothstep(0.05, 0.25, correctedAlignment);
    return safeVoxelNormal(mix(fallback, macroNormal, weight), fallback);
}
