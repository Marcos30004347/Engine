#include "virtualgeometrydata.wgsl"

// The packed geometry identity: the low word is the full instance index, the
// high word the visibility payload (globalCluster << 7 | triangle) shared
// with the software rasterizer.
struct TaaGeometryIds {
    instanceIndex : u32,
    globalCluster : u32,
    triangleId : u32,
}

fn taa_unpack_geometry_ids(lo: u32, hi: u32) -> TaaGeometryIds {
    var ids : TaaGeometryIds;
    ids.instanceIndex = lo;
    ids.globalCluster = visibilityPayloadCluster(hi);
    ids.triangleId = visibilityPayloadTriangle(hi);
    return ids;
}

fn taa_same_surface(a: TaaGeometryIds, b: TaaGeometryIds) -> bool {
    // TAA reuse should survive internal meshlet / cluster boundaries for the same instance.
    // Depth and normal agreement remain the primary validity gates.
    return a.instanceIndex == b.instanceIndex;
}

fn taa_luma(color: vec3<f32>) -> f32 {
    return dot(color, vec3<f32>(0.299, 0.587, 0.114));
}

fn taa_rgb_to_ycocg(color: vec3<f32>) -> vec3<f32> {
    let co = color.r - color.b;
    let tmp = color.b + 0.5 * co;
    let cg = color.g - tmp;
    let y = tmp + 0.5 * cg;
    return vec3<f32>(y, co, cg);
}

fn taa_ycocg_to_rgb(color: vec3<f32>) -> vec3<f32> {
    let tmp = color.x - 0.5 * color.z;
    let g = color.z + tmp;
    let b = tmp - 0.5 * color.y;
    let r = b + color.y;
    return vec3<f32>(r, g, b);
}

fn taa_uv_from_pixel(pixelCoords: vec2<u32>, viewport: vec2<u32>) -> vec2<f32> {
    let viewportSize = vec2<f32>(f32(viewport.x), f32(viewport.y));
    return (vec2<f32>(pixelCoords) + vec2<f32>(0.5)) / viewportSize;
}

fn taa_clip_aabb(history: vec3<f32>, center: vec3<f32>, boxMin: vec3<f32>, boxMax: vec3<f32>) -> vec3<f32> {
    let direction = history - center;
    var t = 1.0;

    if (direction.x > 1e-5) {
        t = min(t, (boxMax.x - center.x) / direction.x);
    } else if (direction.x < -1e-5) {
        t = min(t, (boxMin.x - center.x) / direction.x);
    }

    if (direction.y > 1e-5) {
        t = min(t, (boxMax.y - center.y) / direction.y);
    } else if (direction.y < -1e-5) {
        t = min(t, (boxMin.y - center.y) / direction.y);
    }

    if (direction.z > 1e-5) {
        t = min(t, (boxMax.z - center.z) / direction.z);
    } else if (direction.z < -1e-5) {
        t = min(t, (boxMin.z - center.z) / direction.z);
    }

    return center + direction * clamp(t, 0.0, 1.0);
}

fn taa_pixel_coords(uv: vec2<f32>, viewport: vec2<u32>) -> vec2<i32> {
    let viewportSize = vec2<f32>(f32(viewport.x), f32(viewport.y));
    let pixel = clamp(uv * viewportSize, vec2<f32>(0.0), viewportSize - vec2<f32>(1.0));
    return vec2<i32>(pixel);
}

fn taa_source_pixel_coords(uv: vec2<f32>, viewport: vec2<u32>) -> vec2<i32> {
    let viewportSize = vec2<f32>(f32(viewport.x), f32(viewport.y));
    let pixel = clamp(uv * viewportSize - vec2<f32>(0.5), vec2<f32>(0.0), viewportSize - vec2<f32>(1.0));
    return vec2<i32>(round(pixel));
}

fn taa_catmull_rom_weights(t: f32) -> vec4<f32> {
    let t2 = t * t;
    let t3 = t2 * t;
    return vec4<f32>(
        -0.5 * t + t2 - 0.5 * t3,
        1.0 - 2.5 * t2 + 1.5 * t3,
        0.5 * t + 2.0 * t2 - 1.5 * t3,
        -0.5 * t2 + 0.5 * t3
    );
}

fn taa_load_color_clamped(
    textureInput: texture_2d<f32>,
    pixelCoords: vec2<i32>,
    viewport: vec2<u32>
) -> vec4<f32> {
    let clampedPixel = clamp(
        pixelCoords,
        vec2<i32>(0, 0),
        vec2<i32>(i32(viewport.x) - 1, i32(viewport.y) - 1)
    );
    return textureLoad(textureInput, clampedPixel, 0);
}

fn taa_sample_catmull_rom(
    textureInput: texture_2d<f32>,
    uv: vec2<f32>,
    viewport: vec2<u32>
) -> vec4<f32> {
    let viewportSize = vec2<f32>(f32(viewport.x), f32(viewport.y));
    let samplePos = uv * viewportSize - vec2<f32>(0.5);
    let basePixel = floor(samplePos);
    let frac = samplePos - basePixel;
    let weightsX = taa_catmull_rom_weights(frac.x);
    let weightsY = taa_catmull_rom_weights(frac.y);

    var result = vec4<f32>(0.0);
    for (var y = 0; y < 4; y++) {
        for (var x = 0; x < 4; x++) {
            let pixel = vec2<i32>(basePixel) + vec2<i32>(x - 1, y - 1);
            let weight = weightsX[x] * weightsY[y];
            result += taa_load_color_clamped(textureInput, pixel, viewport) * weight;
        }
    }

    return result;
}

