struct ImageSummary {
    mean : vec4<f32>,
    minimum : vec4<f32>,
    maximum : vec4<f32>,
}

@group(0) @binding(0) var transmittanceLut : texture_2d<f32>;
@group(0) @binding(1) var multipleScatteringLut : texture_2d<f32>;
@group(0) @binding(2) var skyViewLut : texture_2d<f32>;
@group(0) @binding(3) var aerialPerspectiveLut : texture_3d<f32>;
@group(0) @binding(4) var terrainShadowMap : texture_depth_2d;
@group(0) @binding(5) var debugOutput : texture_2d<f32>;
@group(0) @binding(6) var compositeOutput : texture_2d<f32>;
@group(0) @binding(7) var disabledCompositeOutput : texture_2d<f32>;
@group(0) @binding(8) var overviewOutput : texture_2d<f32>;
@group(0) @binding(9) var disabledOverviewOutput : texture_2d<f32>;
@group(0) @binding(10) var<storage, read_write> summaries : array<ImageSummary>;

fn emptySummary() -> ImageSummary {
    return ImageSummary(vec4<f32>(0.0), vec4<f32>(1.0e30),
        vec4<f32>(-1.0e30));
}

fn includeValue(summary : ImageSummary, value : vec4<f32>) -> ImageSummary {
    var result = summary;
    result.mean += value;
    result.minimum = min(result.minimum, value);
    result.maximum = max(result.maximum, value);
    return result;
}

fn finishSummary(summary : ImageSummary, count : u32) -> ImageSummary {
    var result = summary;
    result.mean /= f32(max(count, 1u));
    return result;
}

fn summarizeTransmittance() -> ImageSummary {
    let dimensions = textureDimensions(transmittanceLut);
    var result = emptySummary();
    for (var y = 0u; y < dimensions.y; y += 1u) {
        for (var x = 0u; x < dimensions.x; x += 1u) {
            result = includeValue(result, textureLoad(transmittanceLut,
                vec2<i32>(i32(x), i32(y)), 0));
        }
    }
    return finishSummary(result, dimensions.x * dimensions.y);
}

fn summarizeMultipleScattering() -> ImageSummary {
    let dimensions = textureDimensions(multipleScatteringLut);
    var result = emptySummary();
    for (var y = 0u; y < dimensions.y; y += 1u) {
        for (var x = 0u; x < dimensions.x; x += 1u) {
            result = includeValue(result, textureLoad(multipleScatteringLut,
                vec2<i32>(i32(x), i32(y)), 0));
        }
    }
    return finishSummary(result, dimensions.x * dimensions.y);
}

fn summarizeSkyView() -> ImageSummary {
    let dimensions = textureDimensions(skyViewLut);
    var result = emptySummary();
    for (var y = 0u; y < dimensions.y; y += 1u) {
        for (var x = 0u; x < dimensions.x; x += 1u) {
            result = includeValue(result, textureLoad(skyViewLut,
                vec2<i32>(i32(x), i32(y)), 0));
        }
    }
    return finishSummary(result, dimensions.x * dimensions.y);
}

fn summarizeAerialPerspective() -> ImageSummary {
    let dimensions = textureDimensions(aerialPerspectiveLut);
    var result = emptySummary();
    for (var z = 0u; z < dimensions.z; z += 1u) {
        for (var y = 0u; y < dimensions.y; y += 1u) {
            for (var x = 0u; x < dimensions.x; x += 1u) {
                result = includeValue(result,
                    textureLoad(aerialPerspectiveLut,
                        vec3<i32>(i32(x), i32(y), i32(z)), 0));
            }
        }
    }
    return finishSummary(result,
        dimensions.x * dimensions.y * dimensions.z);
}

fn summarizeTerrainShadow() -> ImageSummary {
    let dimensions = textureDimensions(terrainShadowMap);
    var result = emptySummary();
    for (var y = 0u; y < dimensions.y; y += 1u) {
        for (var x = 0u; x < dimensions.x; x += 1u) {
            let depth = textureLoad(terrainShadowMap,
                vec2<i32>(i32(x), i32(y)), 0);
            result = includeValue(result, vec4<f32>(depth));
        }
    }
    return finishSummary(result, dimensions.x * dimensions.y);
}

fn summarizeDebug() -> ImageSummary {
    let dimensions = textureDimensions(debugOutput);
    var result = emptySummary();
    for (var y = 0u; y < dimensions.y; y += 1u) {
        for (var x = 0u; x < dimensions.x; x += 1u) {
            result = includeValue(result, textureLoad(debugOutput,
                vec2<i32>(i32(x), i32(y)), 0));
        }
    }
    return finishSummary(result, dimensions.x * dimensions.y);
}

fn summarizeComposite() -> ImageSummary {
    let dimensions = textureDimensions(compositeOutput);
    var result = emptySummary();
    for (var y = 0u; y < dimensions.y; y += 1u) {
        for (var x = 0u; x < dimensions.x; x += 1u) {
            result = includeValue(result, textureLoad(compositeOutput,
                vec2<i32>(i32(x), i32(y)), 0));
        }
    }
    return finishSummary(result, dimensions.x * dimensions.y);
}

fn summarizeDisabledComposite() -> ImageSummary {
    let dimensions = textureDimensions(disabledCompositeOutput);
    var result = emptySummary();
    for (var y = 0u; y < dimensions.y; y += 1u) {
        for (var x = 0u; x < dimensions.x; x += 1u) {
            result = includeValue(result, textureLoad(disabledCompositeOutput,
                vec2<i32>(i32(x), i32(y)), 0));
        }
    }
    return finishSummary(result, dimensions.x * dimensions.y);
}

fn summarizeOverview() -> ImageSummary {
    let dimensions = textureDimensions(overviewOutput);
    var result = emptySummary();
    for (var y = 0u; y < dimensions.y; y += 1u) {
        for (var x = 0u; x < dimensions.x; x += 1u) {
            result = includeValue(result, textureLoad(overviewOutput,
                vec2<i32>(i32(x), i32(y)), 0));
        }
    }
    return finishSummary(result, dimensions.x * dimensions.y);
}

fn summarizeDisabledOverview() -> ImageSummary {
    let dimensions = textureDimensions(disabledOverviewOutput);
    var result = emptySummary();
    for (var y = 0u; y < dimensions.y; y += 1u) {
        for (var x = 0u; x < dimensions.x; x += 1u) {
            result = includeValue(result, textureLoad(disabledOverviewOutput,
                vec2<i32>(i32(x), i32(y)), 0));
        }
    }
    return finishSummary(result, dimensions.x * dimensions.y);
}

@compute @workgroup_size(1, 1, 1)
fn inspectAtmospherePasses() {
    if (arrayLength(&summaries) < 10u) { return; }
    summaries[0] = summarizeTransmittance();
    summaries[1] = summarizeMultipleScattering();
    summaries[2] = summarizeSkyView();
    summaries[3] = summarizeAerialPerspective();
    summaries[4] = summarizeTerrainShadow();
    summaries[5] = summarizeDebug();
    summaries[6] = summarizeComposite();
    summaries[7] = summarizeDisabledComposite();
    summaries[8] = summarizeOverview();
    summaries[9] = summarizeDisabledOverview();
}
