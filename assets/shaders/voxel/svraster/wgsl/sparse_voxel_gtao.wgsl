#include "sparse_voxel_types.wgsl"

// Ground-truth ambient occlusion, following Jimenez et al. 2016.
//
// The probe path resolves irradiance at one probe per screen tile, so it
// describes how a room is lit but not what happens in the last few centimetres
// before two surfaces meet. Contact darkening at that scale is below the probe
// grid entirely and no amount of probe density fixes it cheaply. GTAO measures
// it in screen space instead, where the geometry is already at pixel
// resolution.
//
// It is applied to the light buffer *before* material composition, because that
// buffer is a light multiplier at that point in the frame - the same place the
// denoiser operates on. Applying it after composition would darken albedo and
// emission along with the lighting.
//
// "Ground truth" refers to the visibility integral, not the sampling: horizons
// are searched along a small number of screen-space slices, and each slice is
// integrated analytically against the projected normal rather than by counting
// occluded rays. That analytic arc is what makes a handful of slices converge
// to something close to a cosine-weighted visibility integral, where a naive
// horizon-angle average lands well off it.

struct SparseVoxelGTAOSettings {
    // xy raster dimensions, z slices per pixel, w steps per slice direction.
    dimensionsSlices : vec4<u32>,
    // x world-space radius, y strength exponent, z thickness heuristic for
    // rejecting occluders the depth buffer cannot see behind, w frame index.
    parameters : vec4<f32>,
    // xy projection coefficients for viewDepth = y / (deviceDepth + x).
    depthConversion : vec4<f32>,
}

@group(0) @binding(0) var<uniform> raster : SparseVoxelRasterUniforms;
@group(0) @binding(1) var<uniform> settings : SparseVoxelGTAOSettings;
@group(0) @binding(4) var geometryNormalTexture : texture_2d<f32>;
// Separate source and destination, never one texture read-modify-written.
//
// The light buffer this reads is part of a history ring: what it holds this
// frame becomes next frame's history. Multiplying occlusion into it in place
// therefore does not apply AO once, it applies it again to an already occluded
// value every frame - a geometric decay that reads as the image growing darker
// and noisier the longer the camera holds still, and worse with the denoiser
// off because that is the configuration where the ringed buffer is the one
// being written. Writing somewhere else keeps AO a single multiply.
@group(0) @binding(5) var lightingSource : texture_2d<f32>;
@group(0) @binding(6) var lightingTarget : texture_storage_2d<rgba16float, write>;
@group(0) @binding(7) var depthTexture : texture_depth_2d;

fn gtao_camera_direction(pixel : vec2<f32>) -> vec3<f32> {
    return vec3<f32>(
        (pixel.x + 0.5 - raster.focalPrincipal.z) / raster.focalPrincipal.x,
        (pixel.y + 0.5 - raster.focalPrincipal.w) / raster.focalPrincipal.y,
        1.0);
}

// World ray through a pixel centre, matching every other pass's reconstruction.
fn gtao_view_ray(pixel : vec2<f32>) -> vec3<f32> {
    let cameraDirection = gtao_camera_direction(pixel);
    return normalize(
        (raster.cameraToWorld * vec4<f32>(cameraDirection, 0.0)).xyz);
}

// World position of a pixel, or `valid == false` where nothing was rasterised.
struct GTAOSample {
    position : vec3<f32>,
    valid : bool,
}

fn gtao_sample_position(pixel : vec2<i32>, dimensions : vec2<i32>) -> GTAOSample {
    if (any(pixel < vec2<i32>(0)) || any(pixel >= dimensions)) {
        return GTAOSample(vec3<f32>(0.0), false);
    }
    let deviceDepth = textureLoad(depthTexture, pixel, 0);
    if (deviceDepth <= raster.gbufferClearDepth + 1.0e-7) {
        return GTAOSample(vec3<f32>(0.0), false);
    }
    let cameraDirection = gtao_camera_direction(vec2<f32>(pixel));
    let viewDepth = settings.depthConversion.y /
        max(deviceDepth + settings.depthConversion.x, 1.0e-8);
    // Device depth reconstructs axial view depth. Scale by the unnormalised
    // camera ray length to recover distance along the normalised world ray.
    // Reading the final hardware depth makes instanced grass and terrain obey
    // exactly the same GTAO geometry contract.
    let rayDistance = viewDepth * length(cameraDirection);
    let position = raster.cameraWorldPositionNear.xyz +
        gtao_view_ray(vec2<f32>(pixel)) * rayDistance;
    return GTAOSample(position, true);
}

// Interleaved gradient noise. Deliberately *not* animated: the AO term is
// applied after the denoiser, so a per-frame rotation would land as unfiltered
// shimmer rather than being averaged away.
fn gtao_dither(pixel : vec2<f32>) -> f32 {
    return fract(52.9829189 * fract(dot(pixel, vec2<f32>(0.06711056, 0.00583715))));
}

@compute @workgroup_size(8, 8, 1)
fn computeSparseVoxelGTAO(@builtin(global_invocation_id) gid : vec3<u32>) {
    let dimensions = vec2<i32>(max(settings.dimensionsSlices.xy, vec2<u32>(1u)));
    if (any(vec2<i32>(gid.xy) >= dimensions)) { return; }

    // Every path below must write the destination. It is a distinct texture
    // from the source, so an early return would leave the previous frame's
    // contents standing rather than passing the lighting through.
    let passthrough = textureLoad(lightingSource, vec2<i32>(gid.xy), 0);
    let centre = gtao_sample_position(vec2<i32>(gid.xy), dimensions);
    // Uncovered pixels are background. Passing them through unchanged keeps the
    // sky exactly as composed rather than darkening its edges.
    if (!centre.valid) {
        textureStore(lightingTarget, vec2<i32>(gid.xy), passthrough);
        return;
    }

    let normal = normalize(
        textureLoad(geometryNormalTexture, vec2<i32>(gid.xy), 0).xyz);
    let toCamera = normalize(
        raster.cameraWorldPositionNear.xyz - centre.position);
    let radius = max(settings.parameters.x, 1.0e-3);
    let thickness = max(settings.parameters.z, 1.0e-3);

    // Screen-space radius for the world radius at this depth, so the search
    // covers the same world distance whatever the pixel is looking at.
    let depth = max(
        dot(centre.position - raster.cameraWorldPositionNear.xyz,
            normalize(
                (raster.cameraToWorld * vec4<f32>(0.0, 0.0, 1.0, 0.0)).xyz)),
        1.0e-3);
    let screenRadius = radius * raster.focalPrincipal.x / depth;
    // Below a pixel there is nothing to march and every step lands in the same
    // texel, which reads as a hard ring at the distance the radius crosses one
    // pixel. Above a few hundred it is all cache misses for no added contact
    // detail.
    if (screenRadius < 1.0) {
        textureStore(lightingTarget, vec2<i32>(gid.xy), passthrough);
        return;
    }
    let marchRadius = min(screenRadius, 256.0);

    let sliceCount = max(settings.dimensionsSlices.z, 1u);
    let stepCount = max(settings.dimensionsSlices.w, 1u);
    let rotation = gtao_dither(vec2<f32>(gid.xy));
    var visibility = 0.0;
    // Sum of the per-slice weights, which is what the slice contributions have
    // to be normalised by.
    var weightSum = 0.0;

    for (var slice = 0u; slice < sliceCount; slice += 1u) {
        let phi = 3.14159265359 *
            (f32(slice) + rotation) / f32(sliceCount);
        let direction = vec2<f32>(cos(phi), sin(phi));

        // The plane this slice integrates in is spanned by the view direction
        // and the slice direction lifted into world space.
        let sliceWorld = normalize(
            (raster.cameraToWorld *
                vec4<f32>(direction.x / raster.focalPrincipal.x,
                          direction.y / raster.focalPrincipal.y, 0.0, 0.0)).xyz);
        let planeNormal = normalize(cross(sliceWorld, toCamera));
        // Normal projected into the slice plane. Its length is how much of the
        // hemisphere this slice actually represents, and it is the weight the
        // slice contributions are normalised by.
        let projected = normal - planeNormal * dot(normal, planeNormal);
        let projectedLength = length(projected);
        if (projectedLength < 1.0e-4) { continue; }
        let projectedNormal = projected / projectedLength;

        // Signed angle of the projected normal from the view direction, which
        // is what the horizons below are measured relative to.
        let tangent = normalize(cross(planeNormal, toCamera));
        let n = atan2(
            dot(projectedNormal, tangent), dot(projectedNormal, toCamera));

        var horizons = vec2<f32>(-1.0, -1.0);
        for (var side = 0u; side < 2u; side += 1u) {
            let sign = select(1.0, -1.0, side == 1u);
            for (var step = 1u; step <= stepCount; step += 1u) {
                // Quadratic step spacing: contact occlusion is what this is
                // for, so the samples belong near the centre rather than spread
                // evenly out to the radius.
                let fraction = f32(step) / f32(stepCount);
                let offset = direction * sign * marchRadius *
                    fraction * fraction;
                let samplePixel = vec2<i32>(
                    vec2<f32>(gid.xy) + offset + vec2<f32>(0.5));
                let neighbour = gtao_sample_position(samplePixel, dimensions);
                if (!neighbour.valid) { continue; }
                let delta = neighbour.position - centre.position;
                let distance = length(delta);
                if (distance < 1.0e-5) { continue; }
                // A sample beyond the radius says nothing about occlusion here.
                // The thickness term additionally releases occluders that are
                // far behind the surface: the depth buffer has no idea how deep
                // they are, and treating them as solid is what makes distant
                // silhouettes cast haloes onto everything in front of them.
                let falloff = clamp(
                    1.0 - (distance - radius * thickness) /
                        max(radius * (1.0 - thickness), 1.0e-3),
                    0.0, 1.0);
                let cosine = dot(delta / distance, toCamera);
                horizons[side] = max(
                    horizons[side], mix(-1.0, cosine, falloff));
            }
        }

        // Analytic arc integral. The horizons are clamped to the hemisphere
        // around the projected normal before integrating, which is what stops a
        // horizon behind the surface from removing light the surface could
        // never have received anyway.
        let h1 = n + max(-acos(clamp(horizons.x, -1.0, 1.0)) - n, -1.57079632679);
        let h2 = n + min(acos(clamp(horizons.y, -1.0, 1.0)) - n, 1.57079632679);
        let sinN = sin(n);
        let cosN = cos(n);
        let arc =
            0.25 * (-cos(2.0 * h1 - n) + cosN + 2.0 * h1 * sinN) +
            0.25 * (-cos(2.0 * h2 - n) + cosN + 2.0 * h2 * sinN);
        visibility += projectedLength * arc;
        weightSum += projectedLength;
    }

    // Normalise by the accumulated weight, never by the slice count.
    //
    // A slice's contribution is already scaled by how much of the hemisphere it
    // represents - the length of the normal projected into that slice's plane -
    // and that is at most one and usually less. Dividing by the slice count
    // instead treats every skipped or grazing slice as fully visible *and*
    // fully occluding at once, so the result is biased dark everywhere and, on
    // a pixel where every slice was too grazing to sample, collapses to zero -
    // a black dot on an otherwise lit surface.
    if (weightSum < 1.0e-4) {
        textureStore(lightingTarget, vec2<i32>(gid.xy), passthrough);
        return;
    }
    visibility = clamp(visibility / weightSum, 0.0, 1.0);
    // The exponent is the intensity dial: it stays anchored at fully open and
    // fully closed and only bends the response between them, which a linear
    // scale cannot do without either clipping or lifting the black point.
    let occlusion = pow(visibility, max(settings.parameters.y, 0.01));

    textureStore(
        lightingTarget, vec2<i32>(gid.xy),
        vec4<f32>(passthrough.xyz * occlusion, passthrough.w));
}
