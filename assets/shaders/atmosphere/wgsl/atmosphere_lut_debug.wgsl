struct AtmosphereLutDebugUniforms {
    // xy output size, z view, w logarithmic display.
    viewportMode : vec4<u32>,
    // x aerial slice [0,1], y display scale.
    display : vec4<f32>,
}

@group(0) @binding(0) var<uniform> uniforms : AtmosphereLutDebugUniforms;
@group(0) @binding(1) var transmittanceLut : texture_2d<f32>;
@group(0) @binding(2) var multipleScatteringLut : texture_2d<f32>;
@group(0) @binding(3) var skyViewLut : texture_2d<f32>;
@group(0) @binding(4) var aerialPerspectiveLut : texture_3d<f32>;
@group(0) @binding(5) var outputTexture : texture_storage_2d<rgba16float, write>;

const ATMOSPHERE_DEBUG_DISABLED : u32 = 0u;
const ATMOSPHERE_DEBUG_TRANSMITTANCE : u32 = 1u;
const ATMOSPHERE_DEBUG_MULTIPLE_SCATTERING : u32 = 2u;
const ATMOSPHERE_DEBUG_SKY_VIEW : u32 = 3u;
const ATMOSPHERE_DEBUG_AERIAL_RADIANCE : u32 = 4u;
const ATMOSPHERE_DEBUG_AERIAL_TRANSMITTANCE : u32 = 5u;

fn sourcePixel2d(pixel : vec2<u32>, dimensions : vec2<u32>) -> vec2<i32> {
    let normalized = (vec2<f32>(pixel) + vec2<f32>(0.5)) /
        vec2<f32>(uniforms.viewportMode.xy);
    return vec2<i32>(min(
        vec2<u32>(normalized * vec2<f32>(dimensions)),
        dimensions - vec2<u32>(1u)));
}

fn displayColor(value : vec3<f32>) -> vec3<f32> {
    let scaled = max(value * max(uniforms.display.y, 0.0), vec3<f32>(0.0));
    if (uniforms.viewportMode.w != 0u) {
        return log2(vec3<f32>(1.0) + scaled) / 8.0;
    }
    return scaled;
}

@compute @workgroup_size(8, 8, 1)
fn presentAtmosphereLut(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let pixel = globalId.xy;
    if (any(pixel >= uniforms.viewportMode.xy)) {
        return;
    }
    let view = uniforms.viewportMode.z;
    var value = vec4<f32>(0.0, 0.0, 0.0, 1.0);
    if (view == ATMOSPHERE_DEBUG_TRANSMITTANCE) {
        let dimensions = textureDimensions(transmittanceLut);
        value = textureLoad(transmittanceLut,
            sourcePixel2d(pixel, dimensions), 0);
    } else if (view == ATMOSPHERE_DEBUG_MULTIPLE_SCATTERING) {
        let dimensions = textureDimensions(multipleScatteringLut);
        value = textureLoad(multipleScatteringLut,
            sourcePixel2d(pixel, dimensions), 0);
        // Multiple-scattering alpha is the denominator-clamp diagnostic.
        // Magenta makes dense artistic profiles that approach f_ms=1 obvious
        // instead of silently hiding the energy-conservation safeguard.
        value = vec4<f32>(mix(value.rgb, vec3<f32>(1.0, 0.0, 1.0),
            select(0.0, 0.75, value.a > 0.5)), value.a);
    } else if (view == ATMOSPHERE_DEBUG_SKY_VIEW) {
        let dimensions = textureDimensions(skyViewLut);
        value = textureLoad(skyViewLut,
            sourcePixel2d(pixel, dimensions), 0);
    } else if (view == ATMOSPHERE_DEBUG_AERIAL_RADIANCE ||
               view == ATMOSPHERE_DEBUG_AERIAL_TRANSMITTANCE) {
        let dimensions = textureDimensions(aerialPerspectiveLut);
        let source2d = sourcePixel2d(pixel, dimensions.xy);
        let slice = i32(round(clamp(uniforms.display.x, 0.0, 1.0) *
            f32(max(dimensions.z, 1u) - 1u)));
        value = textureLoad(aerialPerspectiveLut,
            vec3<i32>(source2d, slice), 0);
        if (view == ATMOSPHERE_DEBUG_AERIAL_TRANSMITTANCE) {
            value = vec4<f32>(vec3<f32>(value.a), 1.0);
        }
    }
    let color = select(vec3<f32>(0.0), displayColor(value.rgb),
        view != ATMOSPHERE_DEBUG_DISABLED);
    textureStore(outputTexture, vec2<i32>(pixel), vec4<f32>(color, 1.0));
}
