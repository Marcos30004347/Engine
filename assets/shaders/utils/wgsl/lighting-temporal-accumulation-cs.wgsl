struct LightingTemporalAccumulationUniforms {
    // xy: viewport, z: accumulation enabled, w: history valid.
    viewport: vec4<u32>,
    // x: maximum accumulated frames, y: minimum history normal cosine,
    // z: maximum world-space separation in pixel footprints,
    // w: the unjittered camera transform is stationary.
    params: vec4<f32>,
    // x: current-frame same-surface fallback radius in pixels. Used only for
    // covered unresolved sparse-window texels after history reprojection fails.
    // y: receiver screen motion, in pixels, at which the shortened
    // accumulation window starts to apply. z: that window, in measurements.
    // Zero on either restores the single fixed window.
    params2: vec4<f32>,
    // All three transforms operate around the previous camera origin. This
    // avoids reconstructing a ~260 km absolute world position only to cancel
    // it in the previous view matrix, which loses close-ground precision.
    current_clip_to_history_view: mat4x4<f32>,
    history_clip_to_history_view: mat4x4<f32>,
    current_clip_to_history_clip: mat4x4<f32>,
}

@group(0) @binding(0) var<uniform> uniforms: LightingTemporalAccumulationUniforms;
@group(0) @binding(1) var currentLighting: texture_2d<f32>;
@group(0) @binding(2) var historyLighting: texture_2d<f32>;
@group(0) @binding(3) var currentDepth: texture_depth_2d;
@group(0) @binding(4) var currentNormal: texture_2d<f32>;
@group(0) @binding(5) var historyDepth: texture_depth_2d;
@group(0) @binding(6) var historyNormal: texture_2d<f32>;
@group(0) @binding(7) var currentMotion: texture_2d<f32>;
@group(0) @binding(8) var outputLighting: texture_storage_2d<rgba16float, write>;

fn current_estimate_valid(alpha: f32) -> bool {
    // ReSTIR reconstruction contract: 1 is measured, -1 is a compatible
    // current-frame estimate, -2 is covered but unresolved, and 0 is
    // uncovered. Only the first two are Monte Carlo estimates.
    return alpha > 1.0e-5 || (alpha < -0.5 && alpha > -1.5);
}

fn lighting_luminance(value: vec3<f32>) -> f32 {
    return max(dot(value, vec3<f32>(0.2126, 0.7152, 0.0722)), 0.0);
}

fn normalized_normal(texture: texture_2d<f32>, pixel: vec2<i32>) -> vec3<f32> {
    let value = textureLoad(texture, pixel, 0).xyz;
    let length_squared = dot(value, value);
    return select(
        vec3<f32>(0.0, 1.0, 0.0),
        value * inverseSqrt(length_squared),
        length_squared > 1.0e-8);
}

fn position_from_depth(
    pixel: vec2<i32>, depth: f32,
    clip_to_position: mat4x4<f32>) -> vec3<f32> {
    let dimensions = vec2<f32>(max(uniforms.viewport.xy, vec2<u32>(1u)));
    let uv = (vec2<f32>(pixel) + vec2<f32>(0.5)) / dimensions;
    let clip = vec4<f32>(
        uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    let position = clip_to_position * clip;
    return position.xyz / max(abs(position.w), 1.0e-8) * sign(position.w);
}

fn position_pixel_footprint(
    pixel: vec2<i32>, depth: f32,
    clip_to_position: mat4x4<f32>) -> f32 {
    let dimensions = vec2<i32>(max(uniforms.viewport.xy, vec2<u32>(1u)));
    let x_offset = select(
        vec2<i32>(1, 0), vec2<i32>(-1, 0), pixel.x + 1 >= dimensions.x);
    let y_offset = select(
        vec2<i32>(0, 1), vec2<i32>(0, -1), pixel.y + 1 >= dimensions.y);
    let center = position_from_depth(pixel, depth, clip_to_position);
    let x_position = position_from_depth(
        clamp(pixel + x_offset, vec2<i32>(0), dimensions - vec2<i32>(1)),
        depth, clip_to_position);
    let y_position = position_from_depth(
        clamp(pixel + y_offset, vec2<i32>(0), dimensions - vec2<i32>(1)),
        depth, clip_to_position);
    return max(
        length(x_position - center), length(y_position - center));
}

fn history_matches_surface(
    current_pixel: vec2<i32>, history_pixel: vec2<i32>) -> bool {
    let current_depth = textureLoad(currentDepth, current_pixel, 0);
    let history_depth = textureLoad(historyDepth, history_pixel, 0);
    if ((current_depth <= 1.0e-7) != (history_depth <= 1.0e-7)) {
        return false;
    }
    let current_normal = normalized_normal(currentNormal, current_pixel);
    let history_normal = normalized_normal(historyNormal, history_pixel);
    if (dot(current_normal, history_normal) < uniforms.params.y) {
        return false;
    }

    // The ordinary stationary-camera case should depend only on the two
    // stable G-buffer signals: depth and normal. In reverse-Z, a small
    // relative depth tolerance handles quantization without bridging visibly
    // separate layers. This fast path is also immune to large-world matrix
    // reconstruction precision during terrain page publication.
    let depth_tolerance = max(
        1.0e-5,
        max(abs(current_depth), abs(history_depth)) * 0.02);
    if (uniforms.params.w > 0.5 &&
        abs(current_depth - history_depth) <= depth_tolerance) {
        return true;
    }

    // Under camera motion, equal device depth does not imply the same surface:
    // a wrong tap on a coplanar voxel often has exactly the same value. Compare
    // both reconstructed points in the previous camera's relative view frame.
    let current_position = position_from_depth(
        current_pixel, current_depth,
        uniforms.current_clip_to_history_view);
    let history_position = position_from_depth(
        history_pixel, history_depth,
        uniforms.history_clip_to_history_view);
    let footprint = max(
        position_pixel_footprint(
            current_pixel, current_depth,
            uniforms.current_clip_to_history_view),
        position_pixel_footprint(
            history_pixel, history_depth,
            uniforms.history_clip_to_history_view));
    // These coordinates are camera-relative, so their numerical floor can be
    // sub-millimetre rather than the centimetres required by absolute world
    // coordinates. The ordinary tolerance still scales by pixel footprint.
    let tolerance = max(0.0005, footprint * uniforms.params.z);
    let delta = history_position - current_position;
    if (length(delta) > tolerance) { return false; }
    // delta is expressed in the previous camera's view frame, while G-buffer
    // normals are world-space. Dotting them together made acceptance vary with
    // camera rotation. The bounded 3D separation above already provides the
    // necessary positional gate; normal compatibility was tested separately.
    return true;
}

fn history_matches_normal_and_coverage(
    current_pixel: vec2<i32>, history_pixel: vec2<i32>) -> bool {
    let current_depth = textureLoad(currentDepth, current_pixel, 0);
    let history_depth = textureLoad(historyDepth, history_pixel, 0);
    if ((current_depth <= 1.0e-7) != (history_depth <= 1.0e-7)) {
        return false;
    }
    return dot(
        normalized_normal(currentNormal, current_pixel),
        normalized_normal(historyNormal, history_pixel)) >= uniforms.params.y;
}

struct ReprojectedLighting {
    value: vec4<f32>,
    valid: bool,
}

fn current_matches_surface(
    current_pixel: vec2<i32>, sample_pixel: vec2<i32>) -> bool {
    let current_depth = textureLoad(currentDepth, current_pixel, 0);
    let sample_depth = textureLoad(currentDepth, sample_pixel, 0);
    if (current_depth <= 1.0e-7 || sample_depth <= 1.0e-7) {
        return false;
    }

    let current_normal = normalized_normal(currentNormal, current_pixel);
    let sample_normal = normalized_normal(currentNormal, sample_pixel);
    if (dot(current_normal, sample_normal) < uniforms.params.y) {
        return false;
    }

    // Same-frame sparse reconstruction uses only the current G-buffer. A
    // depth-only gate is not enough on sloped terrain under reverse-Z, so use
    // the same camera-relative position metric as temporal reprojection, with
    // a tolerance wide enough to span the selected sparse window while still
    // respecting depth/normal discontinuities.
    let current_position = position_from_depth(
        current_pixel, current_depth,
        uniforms.current_clip_to_history_view);
    let sample_position = position_from_depth(
        sample_pixel, sample_depth,
        uniforms.current_clip_to_history_view);
    let footprint = max(
        position_pixel_footprint(
            current_pixel, current_depth,
            uniforms.current_clip_to_history_view),
        position_pixel_footprint(
            sample_pixel, sample_depth,
            uniforms.current_clip_to_history_view));
    let tolerance_pixels = max(uniforms.params.z, uniforms.params2.x + 0.5);
    let tolerance = max(0.0005, footprint * tolerance_pixels);
    return length(sample_position - current_position) <= tolerance;
}

fn current_frame_fallback(pixel: vec2<i32>) -> ReprojectedLighting {
    let dimensions = vec2<i32>(max(uniforms.viewport.xy, vec2<u32>(1u)));
    let radius = clamp(i32(round(uniforms.params2.x)), 0i, 8i);
    if (radius <= 0i) {
        return ReprojectedLighting(vec4<f32>(0.0), false);
    }

    var sum = vec3<f32>(0.0);
    var weight_sum = 0.0;
    let radius_f = f32(radius);
    let sigma = max(radius_f * 0.5, 1.0);
    let max_distance_squared = radius * radius;
    for (var y = -8i; y <= 8i; y = y + 1i) {
        for (var x = -8i; x <= 8i; x = x + 1i) {
            let tap = pixel + vec2<i32>(x, y);
            if (abs(y) <= radius && abs(x) <= radius &&
                all(tap >= vec2<i32>(0i)) && all(tap < dimensions) &&
                current_matches_surface(pixel, tap)) {
                let sample = textureLoad(currentLighting, tap, 0);
                let distance_squared_i = x * x + y * y;
                if (current_estimate_valid(sample.a) &&
                    distance_squared_i <= max_distance_squared) {
                    let distance_squared = f32(distance_squared_i);
                    let weight = exp(
                        -0.5 * distance_squared /
                        max(sigma * sigma, 1.0e-5));
                    sum = sum + max(sample.rgb, vec3<f32>(0.0)) * weight;
                    weight_sum = weight_sum + weight;
                }
            }
        }
    }
    if (weight_sum <= 1.0e-5) {
        return ReprojectedLighting(vec4<f32>(0.0), false);
    }
    // This is promoted to a one-sample lighting estimate so the temporal
    // accumulator can keep it alive on the next frame. Without that seed,
    // sparse phases that enter during fast motion compose as black until their
    // own ray happens to fire.
    return ReprojectedLighting(vec4<f32>(sum / weight_sum, 1.0), true);
}

fn catmull_rom_weights(t: f32) -> vec4<f32> {
    let t2 = t * t;
    let t3 = t2 * t;
    return vec4<f32>(
        -0.5 * t + t2 - 0.5 * t3,
        1.0 - 2.5 * t2 + 1.5 * t3,
        0.5 * t + 2.0 * t2 - 1.5 * t3,
        -0.5 * t2 + 0.5 * t3);
}

fn reprojected_history_at(
    pixel: vec2<i32>, history_uv: vec2<f32>,
    current: vec4<f32>) -> ReprojectedLighting {
    let dimensions = vec2<i32>(max(uniforms.viewport.xy, vec2<u32>(1u)));
    let dimensions_f = vec2<f32>(dimensions);
    if (any(history_uv < vec2<f32>(0.0)) ||
        any(history_uv >= vec2<f32>(1.0))) {
        return ReprojectedLighting(vec4<f32>(0.0), false);
    }

    let history_position = history_uv * dimensions_f - vec2<f32>(0.5);
    let base = vec2<i32>(floor(history_position));
    let fraction = fract(history_position);
    var sum = vec4<f32>(0.0);
    var weight_sum = 0.0;
    for (var y = 0; y <= 1; y = y + 1) {
        for (var x = 0; x <= 1; x = x + 1) {
            let tap = base + vec2<i32>(x, y);
            if (any(tap < vec2<i32>(0)) || any(tap >= dimensions) ||
                !history_matches_surface(pixel, tap)) {
                continue;
            }
            let sample = textureLoad(historyLighting, tap, 0);
            if (sample.a <= 1.0e-5) { continue; }
            let weight =
                select(1.0 - fraction.x, fraction.x, x == 1) *
                select(1.0 - fraction.y, fraction.y, y == 1);
            sum = sum + sample * weight;
            weight_sum = weight_sum + weight;
        }
    }
    if (weight_sum <= 1.0e-5) {
        // Geometric reprojection for a small camera rotation can land between
        // sparse history samples. If the ordinary bilinear footprint contains no
        // valid accumulated ray, gather a tiny surface-compatible neighbourhood
        // before declaring the lightfield history lost. This intentionally
        // allows a little blur in the fallback path because keeping compatible
        // noisy lightfield evidence is preferable to dropping to an unresolved
        // black texel. The ordinary bilinear/Catmull path above remains the
        // sharp path whenever it has any directly reprojected history.
        var fallback_sum = vec4<f32>(0.0);
        var fallback_weight_sum = 0.0;
        for (var y = -2; y <= 2; y = y + 1) {
            for (var x = -2; x <= 2; x = x + 1) {
                let tap = base + vec2<i32>(x, y);
                if (any(tap < vec2<i32>(0)) || any(tap >= dimensions) ||
                    !history_matches_surface(pixel, tap)) {
                    continue;
                }
                let sample = textureLoad(historyLighting, tap, 0);
                if (sample.a <= 1.0e-5) { continue; }
                let delta = (vec2<f32>(tap) + vec2<f32>(0.5)) -
                    history_uv * dimensions_f;
                let distance_squared = dot(delta, delta);
                if (distance_squared > 4.0001) { continue; }
                // A compact Gaussian-ish kernel: close compatible samples
                // dominate, but a two-pixel sparse phase still contributes
                // enough to keep the temporal signal alive.
                let weight = exp(-0.5 * distance_squared);
                fallback_sum = fallback_sum + sample * weight;
                fallback_weight_sum = fallback_weight_sum + weight;
            }
        }
        if (fallback_weight_sum > 1.0e-5) {
            return ReprojectedLighting(
                fallback_sum / fallback_weight_sum, true);
        }
        return ReprojectedLighting(vec4<f32>(0.0), false);
    }

    let bilinear = sum / weight_sum;

    // Recursive bilinear history sampling is a low-pass filter: even perfect
    // reprojection softens a sharp shadow on every sub-pixel camera rotation.
    // On a completely compatible 4x4 surface footprint, reconstruct radiance
    // with a Catmull-Rom kernel instead. Its negative lobes retain spatial
    // frequencies that bilinear sampling discards. Clamp to the footprint's
    // observed range to prevent ringing, and retain bilinear confidence since
    // sample count is not a radiance signal. Any geometry/disocclusion mismatch
    // falls back to the conservative bilinear result above.
    let weights_x = catmull_rom_weights(fraction.x);
    let weights_y = catmull_rom_weights(fraction.y);
    var cubic_rgb = vec3<f32>(0.0);
    var minimum_rgb = vec3<f32>(3.4028235e38);
    var maximum_rgb = vec3<f32>(0.0);
    var cubic_compatible = true;
    for (var y = -1; y <= 2; y = y + 1) {
        for (var x = -1; x <= 2; x = x + 1) {
            let weight =
                weights_x[u32(x + 1)] * weights_y[u32(y + 1)];
            // At an exact pixel coordinate the outer Catmull taps have zero
            // weight. They neither need a texture read nor make a viewport
            // edge incompatible with the separable reconstruction.
            if (abs(weight) <= 1.0e-7) { continue; }
            let tap = base + vec2<i32>(x, y);
            if (any(tap < vec2<i32>(0)) || any(tap >= dimensions) ||
                !history_matches_normal_and_coverage(pixel, tap)) {
                cubic_compatible = false;
                continue;
            }
            let sample = textureLoad(historyLighting, tap, 0);
            if (sample.a <= 1.0e-5) {
                cubic_compatible = false;
                continue;
            }
            let rgb = max(sample.rgb, vec3<f32>(0.0));
            cubic_rgb = cubic_rgb + rgb * weight;
            minimum_rgb = min(minimum_rgb, rgb);
            maximum_rgb = max(maximum_rgb, rgb);
        }
    }
    if (cubic_compatible) {
        return ReprojectedLighting(
            vec4<f32>(clamp(cubic_rgb, minimum_rgb, maximum_rgb), bilinear.a),
            true);
    }
    return ReprojectedLighting(bilinear, true);
}

fn reprojected_history(
    pixel: vec2<i32>, current: vec4<f32>) -> ReprojectedLighting {
    if (uniforms.viewport.w == 0u) {
        return ReprojectedLighting(vec4<f32>(0.0), false);
    }
    let dimensions = vec2<f32>(max(uniforms.viewport.xy, vec2<u32>(1u)));
    let uv = (vec2<f32>(pixel) + vec2<f32>(0.5)) / dimensions;

    // A stationary camera cannot move a static terrain receiver to another
    // pixel. Prefer its exact history coordinate before consulting object
    // motion, which may contain projection jitter or transient page motion.
    // Testing motion first lets a bogus but depth/normal-compatible vector
    // drag binary shadow history sideways across coplanar ground.
    if (uniforms.params.w > 0.5) {
        let stationary_history = reprojected_history_at(pixel, uv, current);
        if (stationary_history.valid) { return stationary_history; }
    }

    // Static terrain is most reliably reprojected geometrically. The supplied
    // transform maps current clip directly into previous clip through a small
    // camera-relative view transform; it never forms an absolute world point.
    // Attempt this before motion because a stale vector on broad coplanar
    // terrain can otherwise land on the wrong, depth-compatible receiver.
    let current_depth = textureLoad(currentDepth, pixel, 0);
    let current_clip = vec4<f32>(
        uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0,
        current_depth, 1.0);
    let previous_clip = uniforms.current_clip_to_history_clip * current_clip;
    if (abs(previous_clip.w) > 1.0e-8) {
        let previous_ndc = previous_clip.xy / previous_clip.w;
        let camera_uv = vec2<f32>(
            previous_ndc.x * 0.5 + 0.5,
            0.5 - previous_ndc.y * 0.5);
        let camera_history = reprojected_history_at(
            pixel, camera_uv, current);
        if (camera_history.valid) { return camera_history; }
    }

    // Motion remains a fallback for genuinely moving receivers and for cases
    // where the geometric camera reprojection leaves the previous viewport.
    let motion_uv = uv - textureLoad(currentMotion, pixel, 0).xy;
    return reprojected_history_at(pixel, motion_uv, current);
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    if (any(global_id.xy >= uniforms.viewport.xy)) { return; }
    let pixel = vec2<i32>(global_id.xy);
    let current = textureLoad(currentLighting, pixel, 0);

    // Debug/training views require the exact current-frame contract. Keeping
    // this bypass inside the accumulator lets the graph topology stay fixed.
    if (uniforms.viewport.z == 0u) {
        if (current.a < -1.5) {
            let fallback = current_frame_fallback(pixel);
            if (fallback.valid) {
                textureStore(outputLighting, pixel, fallback.value);
                return;
            }
        }
        textureStore(outputLighting, pixel,
            vec4<f32>(max(current.rgb, vec3<f32>(0.0)), current.a));
        return;
    }

    let current_valid = current_estimate_valid(current.a);
    let reprojected = reprojected_history(pixel, current);
    let history = reprojected.value;
    let history_valid = reprojected.valid;

    if (!current_valid) {
        // A rotating sparse schedule may not reach a covered edge in one
        // frame. Preserve an already validated estimate there, but never turn
        // an uncovered pixel into lit geometry.
        if (current.a < -1.5 && history_valid) {
            textureStore(outputLighting, pixel, history);
        } else if (current.a < -1.5) {
            let fallback = current_frame_fallback(pixel);
            textureStore(outputLighting, pixel,
                select(
                    vec4<f32>(max(current.rgb, vec3<f32>(0.0)), current.a),
                    fallback.value,
                    fallback.valid));
        } else {
            textureStore(outputLighting, pixel,
                vec4<f32>(max(current.rgb, vec3<f32>(0.0)), current.a));
        }
        return;
    }

    // Alpha -1 is a spatial estimate used only to fill this frame's sparse
    // window. It is not a new observation and therefore must neither pull a
    // valid temporal estimate toward the filter result nor acquire temporal
    // confidence of its own. This is especially important while rotating:
    // otherwise the spatial blur is fed back into history on every frame.
    if (current.a < -0.5) {
        textureStore(outputLighting, pixel,
            select(current, history, history_valid));
        return;
    }

    if (!history_valid) {
        textureStore(outputLighting, pixel,
            vec4<f32>(max(current.rgb, vec3<f32>(0.0)), 1.0));
        return;
    }

    // Successful reprojection means this is still the same receiver. Camera
    // motion must not collapse a converged 32-frame estimate to four samples:
    // doing so makes variance jump on the first sub-pixel rotation and leaves
    // the spatial filter to manufacture a blurry replacement. Real lighting
    // changes remain responsive through the delta gate below, while geometry
    // changes are rejected by the surface test above.
    let maximum_frames = max(uniforms.params.x, 1.0);
    let history_count = clamp(history.a, 1.0, maximum_frames);
    let current_luma = lighting_luminance(max(current.rgb, vec3<f32>(0.0)));
    let history_luma = lighting_luminance(max(history.rgb, vec3<f32>(0.0)));
    let lighting_delta = abs(current_luma - history_luma);
    let responsive_threshold = max(
        0.02, max(current_luma, history_luma) * 0.25);
    let next_count = min(history_count + 1.0, maximum_frames);
    // Running mean until the bounded window fills, then an EMA with the same
    // effective window. This converges a stationary camera without allowing
    // unbounded stale history.
    //
    // The window is shortened while this receiver moves on screen. Every frame
    // the camera moves, an unmeasured pixel's estimate is reprojected and
    // resampled onto a new position, and only the scheduled fraction of pixels
    // - one in four at half ray rate - takes a measurement that can correct
    // it, each weighted 1/window. The full window therefore holds roughly a
    // second of pre-motion, repeatedly resampled radiance, and that is what is
    // still on screen the moment the camera stops: a smear that then costs
    // another whole window of measurements to clear. Following the
    // measurements actually taken during the motion spends variance exactly
    // while variance is least visible and leaves a current estimate at the
    // stop. It is inert for a stationary camera, whose motion is zero.
    //
    // Responsiveness only. The published sample count above is deliberately
    // unchanged: it is the confidence SVGF reads to size its spatial filter,
    // and collapsing that on camera motion is what previously replaced the
    // accumulator's own detail with filter blur.
    let motion_pixels = length(
        textureLoad(currentMotion, pixel, 0).xy *
            vec2<f32>(max(uniforms.viewport.xy, vec2<u32>(1u))));
    let motion_start = uniforms.params2.y;
    let motion_window = clamp(uniforms.params2.z, 1.0, maximum_frames);
    let motion_response = select(
        0.0,
        smoothstep(
            motion_start, max(motion_start * 4.0, motion_start + 0.01),
            motion_pixels),
        motion_start > 0.0 && uniforms.params2.z >= 1.0);
    let responsive_window = max(
        mix(maximum_frames, motion_window, motion_response), 1.0);
    let baseline_weight =
        1.0 / min(history_count + 1.0, responsive_window);
    // A lighting change may need a quicker response even though the receiver
    // still matches, but responsiveness is separate from confidence. Resetting
    // the sample count made SVGF interpret every stochastic visibility change
    // as a newly exposed pixel and abruptly increase blur during camera motion.
    // While the camera moves, a large delta is predominantly a newly selected
    // ReSTIR sample, so keep the full-window weight; explicit scene/light
    // epochs still invalidate history when the lighting setup changes.
    let current_weight = max(
        baseline_weight,
        select(
            0.0, 0.125,
            lighting_delta > responsive_threshold &&
                uniforms.params.w > 0.5));
    let accumulated = mix(
        max(history.rgb, vec3<f32>(0.0)),
        max(current.rgb, vec3<f32>(0.0)),
        current_weight);
    textureStore(outputLighting, pixel, vec4<f32>(accumulated, next_count));
}
