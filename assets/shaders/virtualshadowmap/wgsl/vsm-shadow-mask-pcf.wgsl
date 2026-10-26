#include "vsm-shadow-pcf-common.wgsl"

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let dims = textureDimensions(outputTexture);
    if (global_id.x >= dims.x || global_id.y >= dims.y) {
        return;
    }

    let pixel_coord = global_id.xy;
    let depth = textureLoad(depthTexture, vec2<i32>(pixel_coord), 0);
    let ambient_lighting = max(uniforms.ambientShadowColor, vec3<f32>(0.0, 0.0, 0.0));

    var accumulated_lighting = vec3<f32>(0.0, 0.0, 0.0);
    var average_visibility = 1.0;

    if (uniforms.enabled != 0u && uniforms.activeLights > 0u && depth > 0.0) {
        accumulated_lighting = ambient_lighting;
        let camera_distance = length(reconstruct_view_position(pixel_coord, depth));
        let world_position = reconstruct_world_position(pixel_coord);
        var surface_normal = vec3<f32>(0.0, 0.0, 1.0);
        let has_surface_normal = try_load_gbuffer_normal(pixel_coord, &surface_normal);
        var visibility_sum = 0.0;
        var light_count = 0u;
        var light_index = 0u;
        loop {
            light_index = vsm_next_tile_light(light_index,pixel_coord,dims,uniforms.activeLights);
            if (light_index >= uniforms.activeLights) {
                break;
            }

            if (!light_affects_world_position(light_index, world_position)) {
                light_index = light_index + 1u;
                continue;
            }

            let light_radiance_value = light_radiance(light_index);
            if (max(light_radiance_value.x, max(light_radiance_value.y, light_radiance_value.z)) <= 1e-6) {
                light_index = light_index + 1u;
                continue;
            }

            let distance_falloff = light_distance_falloff(light_index, world_position);
            let normal_lighting_weight = evaluate_normal_lighting_weight(light_index, world_position, surface_normal, has_surface_normal);
            let shadow_radiance = shadow_radiance_for_light(light_index, light_radiance_value);
            let distance_shaded_radiance = mix(shadow_radiance, light_radiance_value, distance_falloff);
            let normal_shaded_radiance = mix(shadow_radiance, distance_shaded_radiance, normal_lighting_weight);
            let shadow_weight = vsm_shadow_distance_weight(camera_distance,
                directionalLights[light_index].params.z, directionalLights[light_index].params.w);
            if (shadow_weight <= 0.0) {
                accumulated_lighting += max(normal_shaded_radiance - ambient_lighting, vec3<f32>(0.0));
                visibility_sum += normal_lighting_weight * distance_falloff;
                light_count += 1u; light_index += 1u;
                continue;
            }
            let screen_space_visibility = evaluate_screen_space_shadow(pixel_coord, light_index);
            if (screen_space_visibility <= 0.0 && shadow_weight >= 1.0) {
                light_count = light_count + 1u;
                light_index = light_index + 1u;
                continue;
            }

            let receiver = resolve_receiver_state_for_light(pixel_coord, world_position, camera_distance, light_index);
            if (receiver.valid != 0u) {
                let contact_visibility = evaluate_contact_shadow(pixel_coord, world_position, receiver, light_index);
                let near_field_visibility = min(contact_visibility, screen_space_visibility);
                var shadow_filter_visibility = 1.0;
                if (uniforms.softShadowMethod == SOFT_SHADOW_METHOD_LAYERED_SSSS) {
                    let hard_visibility = sample_shadow_test_current_texel(
                        receiver.resolvedLayer,
                        receiver.virtualTexel,
                        receiver.receiverDepth,
                        receiver.effectiveBias);
                    let ssss_visibility = evaluate_layered_screen_space_soft_shadow(pixel_coord, light_index);
                    let hard_leak_guard = smoothstep(0.98, 1.0, ssss_visibility);
                    shadow_filter_visibility = mix(ssss_visibility, min(ssss_visibility, hard_visibility), hard_leak_guard);
                } else if (uniforms.softShadowMethod == SOFT_SHADOW_METHOD_PCSS) {
                    shadow_filter_visibility = evaluate_pcss(receiver, pixel_coord);
                } else {
                    shadow_filter_visibility = evaluate_pcf(receiver);
                }
                let combined_visibility = mix(1.0, min(shadow_filter_visibility, near_field_visibility), shadow_weight);
                let light_contribution = mix(shadow_radiance, normal_shaded_radiance, combined_visibility);
                accumulated_lighting = accumulated_lighting + max(light_contribution - ambient_lighting, vec3<f32>(0.0, 0.0, 0.0));
                visibility_sum = visibility_sum + combined_visibility * normal_lighting_weight * distance_falloff;
                light_count = light_count + 1u;
            } else {
                let ssss_visibility = evaluate_layered_screen_space_soft_shadow(pixel_coord, light_index);
                let combined_visibility = mix(1.0, min(screen_space_visibility, ssss_visibility), shadow_weight);
                let light_contribution = mix(shadow_radiance, normal_shaded_radiance, combined_visibility);
                accumulated_lighting = accumulated_lighting + max(light_contribution - ambient_lighting, vec3<f32>(0.0, 0.0, 0.0));
                visibility_sum = visibility_sum + combined_visibility * normal_lighting_weight * distance_falloff;
                light_count = light_count + 1u;
            }

            light_index = light_index + 1u;
        }

        if (light_count > 0u) {
            average_visibility = visibility_sum / f32(light_count);
        } else {
            accumulated_lighting = ambient_lighting;
            average_visibility = 0.0;
        }
    } else if (depth > 0.0) {
        accumulated_lighting = vec3<f32>(1.0, 1.0, 1.0);
    }

    textureStore(
        outputTexture,
        vec2<i32>(pixel_coord),
        vec4<f32>(accumulated_lighting, average_visibility));
}
