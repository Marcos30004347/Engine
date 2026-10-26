fn vsm_project_world_to_light_uv(
    world_position: vec3<f32>,
    layer: u32,
    uv_out: ptr<function, vec2<f32>>,
    ndc_out: ptr<function, vec3<f32>>
) -> bool {
    let ndc = vsm_project_world_to_cascade_ndc(world_position, cascadeMatrices[layer].viewProj);
    (*ndc_out) = ndc;
    if (!vsm_cascade_ndc_is_covered(ndc)) {
        return false;
    }

    (*uv_out) = vec2<f32>(ndc.x * 0.5 + 0.5, ndc.y * 0.5 + 0.5);
    return true;
}

fn vsm_reconstruct_world_position_from_pixel_corner(
    pixel_corner: vec2<u32>,
    depth: f32,
    dims: vec2<u32>
) -> vec3<f32> {
    let uv = vec2<f32>(pixel_corner) / vec2<f32>(dims);
    return vsm_reconstruct_world_position_from_uv_depth(uv, depth, cameraState.inverseViewProjection);
}

fn vsm_project_pixel_footprint_uv_bounds(
    pixel_coord: vec2<u32>,
    depth: f32,
    layer: u32,
    uv_min_out: ptr<function, vec2<f32>>,
    uv_max_out: ptr<function, vec2<f32>>
) -> bool {
    let depth_dims = textureDimensions(depthTexture);
    var center_uv = vec2<f32>(0.0, 0.0);
    var center_ndc = vec3<f32>(0.0, 0.0, 0.0);
    if (!vsm_project_world_to_light_uv(
            vsm_reconstruct_world_position_from_uv_depth(vsm_texel_center_uv(pixel_coord, depth_dims), depth, cameraState.inverseViewProjection),
            layer,
            &center_uv,
            &center_ndc)) {
        return false;
    }

    var uv_min = center_uv;
    var uv_max = center_uv;
    let corner_base = pixel_coord;
    let corner_offsets = array<vec2<u32>, 4>(
        vec2<u32>(0u, 0u),
        vec2<u32>(1u, 0u),
        vec2<u32>(0u, 1u),
        vec2<u32>(1u, 1u));

    for (var i = 0u; i < 4u; i++) {
        let corner = min(corner_base + corner_offsets[i], depth_dims);
        var corner_uv = vec2<f32>(0.0, 0.0);
        var corner_ndc = vec3<f32>(0.0, 0.0, 0.0);
        if (vsm_project_world_to_light_uv(
                vsm_reconstruct_world_position_from_pixel_corner(corner, depth, depth_dims),
                layer,
                &corner_uv,
                &corner_ndc)) {
            uv_min = min(uv_min, corner_uv);
            uv_max = max(uv_max, corner_uv);
        }
    }

    (*uv_min_out) = uv_min;
    (*uv_max_out) = uv_max;
    return true;
}

fn vsm_choose_perspective_mip_from_footprint(
    uv_min: vec2<f32>,
    uv_max: vec2<f32>,
    base_page_table_resolution: u32,
    physical_page_size: u32,
    mip_count: u32,
    mip_transition_scale: f32,
    mip_bias: f32,
    page_world_scale: f32
) -> u32 {
    let base_shadow_resolution_texels =
        f32(max(base_page_table_resolution, 1u)) *
        f32(max(physical_page_size, 1u));
    let footprint_texels = max(
        (uv_max - uv_min) * base_shadow_resolution_texels,
        vec2<f32>(1.0, 1.0));
    let max_footprint_texels = max(footprint_texels.x, footprint_texels.y);
    let conservative_scale = max(mip_transition_scale, 1e-4);
    let scaled_footprint_texels = max_footprint_texels * max(page_world_scale, 1e-4);
    let biased_lod = log2(max(scaled_footprint_texels / conservative_scale, 1.0)) + mip_bias;
    let unclamped_mip = u32(max(floor(biased_lod), 0.0));
    return min(unclamped_mip, max(mip_count, 1u) - 1u);
}

fn vsm_select_perspective_layer_from_base_layer(
    pixel_coord: vec2<u32>,
    depth: f32,
    base_layer: u32,
    mip_count: u32,
    physical_page_size: u32,
    mip_transition_scale: f32,
    mip_bias: f32,
    page_world_scale: f32,
    selected_layer_out: ptr<function, u32>,
    base_uv_min_out: ptr<function, vec2<f32>>,
    base_uv_max_out: ptr<function, vec2<f32>>
) -> bool {
    if (base_layer >= uniforms.activeLayers) {
        return false;
    }

    var base_uv_min = vec2<f32>(0.0, 0.0);
    var base_uv_max = vec2<f32>(0.0, 0.0);
    if (!vsm_project_pixel_footprint_uv_bounds(pixel_coord, depth, base_layer, &base_uv_min, &base_uv_max)) {
        return false;
    }

    let desired_mip = vsm_choose_perspective_mip_from_footprint(
        base_uv_min,
        base_uv_max,
        layer_page_table_resolution(base_layer),
        physical_page_size,
        mip_count,
        mip_transition_scale,
        mip_bias,
        page_world_scale);
    let selected_layer = base_layer + desired_mip;
    if (selected_layer >= uniforms.activeLayers) {
        return false;
    }

    (*selected_layer_out) = selected_layer;
    (*base_uv_min_out) = base_uv_min;
    (*base_uv_max_out) = base_uv_max;
    return true;
}
