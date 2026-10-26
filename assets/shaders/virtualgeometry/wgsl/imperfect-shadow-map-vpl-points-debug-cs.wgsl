struct Uniforms {
    viewProjection : mat4x4<f32>,
    viewportVplCountSelected : vec4<u32>,
    radiusExposureEnabled : vec4<f32>,
};

struct VPL {
    positionRange : vec4<f32>,
    normalPdf : vec4<f32>,
    fluxLight : vec4<f32>,
};

@group(0) @binding(0) var<uniform> u : Uniforms;
@group(0) @binding(1) var<storage, read> vpls : array<VPL>;
@group(0) @binding(2) var outputTexture : texture_storage_2d<rgba16float, write>;

fn color_for_vpl(vpl : VPL, selected : bool) -> vec3<f32> {
    let flux = max(vpl.fluxLight.xyz * u.radiusExposureEnabled.y, vec3<f32>(0.0));
    let flux_color = flux / (vec3<f32>(1.0) + flux);
    let normal_color = vpl.normalPdf.xyz * vec3<f32>(0.5) + vec3<f32>(0.5);
    let base = mix(normal_color, flux_color, select(0.35, 0.80, dot(flux, vec3<f32>(1.0)) > 1e-5));
    return select(base, vec3<f32>(1.0, 0.85, 0.25), selected);
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) id : vec3<u32>) {
    let dims = textureDimensions(outputTexture);
    if (id.x >= dims.x || id.y >= dims.y) {
        return;
    }

    var out_color = vec4<f32>(0.0, 0.0, 0.0, 1.0);
    if (u.radiusExposureEnabled.z <= 0.0) {
        textureStore(outputTexture, vec2<i32>(id.xy), out_color);
        return;
    }

    let vpl_count = min(u.viewportVplCountSelected.z, 64u);
    if (vpl_count == 0u) {
        textureStore(outputTexture, vec2<i32>(id.xy), vec4<f32>(0.20, 0.025, 0.02, 1.0));
        return;
    }

    let selected_plus_one = u.viewportVplCountSelected.w;
    let pixel = vec2<f32>(id.xy) + vec2<f32>(0.5, 0.5);
    let radius = max(u.radiusExposureEnabled.x, 1.0);
    let radius2 = radius * radius;
    var best_alpha = 0.0;
    var best_color = out_color.xyz;

    for (var i = 0u; i < 64u; i = i + 1u) {
        if (i >= vpl_count) {
            break;
        }
        if (selected_plus_one != 0u && i + 1u != selected_plus_one) {
            continue;
        }

        let vpl = vpls[i];
        let clip_raw = u.viewProjection * vec4<f32>(vpl.positionRange.xyz, 1.0);
        let clip = vec4<f32>(clip_raw.x, -clip_raw.y, clip_raw.z, clip_raw.w);
        if (clip.w <= 1e-5) {
            continue;
        }

        let ndc = clip.xyz / clip.w;
        if (abs(ndc.x) > 1.1 || abs(ndc.y) > 1.1 || ndc.z < 0.0 || ndc.z > 1.0) {
            continue;
        }

        let screen = vec2<f32>(
            (ndc.x * 0.5 + 0.5) * f32(u.viewportVplCountSelected.x),
            (ndc.y * 0.5 + 0.5) * f32(u.viewportVplCountSelected.y));
        let delta = pixel - screen;
        let dist2 = dot(delta, delta);
        if (dist2 > radius2) {
            continue;
        }

        let selected = selected_plus_one != 0u;
        let core = 1.0 - sqrt(dist2) / radius;
        let alpha = select(smoothstep(0.0, 1.0, core), 1.0, core > 0.65);
        if (alpha >= best_alpha) {
            best_alpha = alpha;
            best_color = color_for_vpl(vpl, selected);
        }
    }

    let final_color = mix(out_color.xyz, best_color, best_alpha);
    out_color = vec4<f32>(final_color, 1.0);
    textureStore(outputTexture, vec2<i32>(id.xy), out_color);
}
