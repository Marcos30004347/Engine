#include "vsm-shadow-pcf-common.wgsl"

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let dims = textureDimensions(outputTexture);
    if (global_id.x >= dims.x || global_id.y >= dims.y) {
        return;
    }

    let pixel_coord = global_id.xy;
    let depth = textureLoad(depthTexture, vec2<i32>(pixel_coord), 0);

    var accumulated_lighting = vec3<f32>(0.0, 0.0, 0.0);
    var average_visibility = 1.0;

    if (uniforms.enabled != 0u && uniforms.activeDirectionalLights > 0u && depth > 0.0) {
        let world_position = reconstruct_world_position(pixel_coord);
        var visibility_sum = 0.0;
        var light_count = 0u;
        var light_index = 0u;
        loop {
            if (light_index >= uniforms.activeDirectionalLights) {
                break;
            }

            let receiver = resolve_receiver_state_for_light(pixel_coord, world_position, light_index);
            let light_color = clamp(directionalLights[light_index].color.rgb, vec3<f32>(0.0), vec3<f32>(1.0));
            if (receiver.valid != 0u) {
                let filtered_visibility = evaluate_pcf(receiver);
                let near_field_visibility = evaluate_screen_space_shadow(pixel_coord, receiver, light_index);
                let combined_visibility = min(filtered_visibility, near_field_visibility);
                let shadow_color = clamp(uniforms.ambientShadowColor * light_color, vec3<f32>(0.0), vec3<f32>(1.0));
                let light_contribution = mix(shadow_color, light_color, combined_visibility);
                accumulated_lighting = vec3<f32>(1.0) - (vec3<f32>(1.0) - accumulated_lighting) * (vec3<f32>(1.0) - light_contribution);
                visibility_sum = visibility_sum + combined_visibility;
                light_count = light_count + 1u;
            } else {
                accumulated_lighting = vec3<f32>(1.0) - (vec3<f32>(1.0) - accumulated_lighting) * (vec3<f32>(1.0) - light_color);
                visibility_sum = visibility_sum + 1.0;
                light_count = light_count + 1u;
            }

            light_index = light_index + 1u;
        }

        if (light_count > 0u) {
            average_visibility = visibility_sum / f32(light_count);
        }
    } else if (depth > 0.0) {
        accumulated_lighting = vec3<f32>(1.0, 1.0, 1.0);
    }

    textureStore(
        outputTexture,
        vec2<i32>(pixel_coord),
        vec4<f32>(accumulated_lighting, average_visibility));
}
