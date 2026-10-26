struct NavigationMergeUniforms {
    viewportBlend : vec4<f32>,
    skyHorizonIntensity : vec4<f32>,
    skyZenithExponent : vec4<f32>,
    skyGround : vec4<f32>,
    cameraDirectionFocal : vec4<f32>,
    cameraRightStars : vec4<f32>,
    cameraUpControls : vec4<f32>,
    sunDirectionIntensity : vec4<f32>,
    sunColorAngularRadius : vec4<f32>,
    sunDisplay : vec4<f32>,
}

@group(0) @binding(0) var surfaceColor : texture_2d<f32>;
@group(0) @binding(1) var surfaceDepth : texture_depth_2d;
@group(0) @binding(2) var overviewColor : texture_2d<f32>;
@group(0) @binding(3) var overviewDepth : texture_depth_2d;
@group(0) @binding(4) var<uniform> uniforms : NavigationMergeUniforms;
@group(0) @binding(5) var overviewAtmosphere : texture_2d<f32>;

struct MergeVertexOutput {
    @builtin(position) position : vec4<f32>,
    @location(0) uv : vec2<f32>,
}

@vertex
fn drawNavigationMergeVertex(@builtin(vertex_index) vertexIndex : u32) -> MergeVertexOutput {
    let uv = vec2<f32>(f32((vertexIndex << 1u) & 2u), f32(vertexIndex & 2u));
    var output : MergeVertexOutput;
    output.position = vec4<f32>(uv * 2.0 - 1.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}

fn scaledPixel(uv : vec2<f32>, dimensions : vec2<u32>) -> vec2<i32> {
    return vec2<i32>(min(
        vec2<u32>(uv * vec2<f32>(dimensions)), dimensions - vec2<u32>(1u)));
}

fn navigation_star_hash(value : vec2<f32>) -> vec3<f32> {
    return fract(sin(vec3<f32>(
        dot(value, vec2<f32>(127.1, 311.7)),
        dot(value, vec2<f32>(269.5, 183.3)),
        dot(value, vec2<f32>(419.2, 371.9)))) * 43758.5453);
}

fn navigation_star_layer(direction : vec3<f32>, resolution : f32,
    threshold : f32) -> vec3<f32> {
    let pi = 3.14159265358979323846;
    let uv = vec2<f32>(atan2(direction.z, direction.x) / (2.0 * pi) + 0.5,
        asin(clamp(direction.y, -1.0, 1.0)) / pi + 0.5);
    let grid = vec2<f32>(resolution, resolution * 0.5);
    let coordinate = uv * grid;
    let cell = floor(coordinate);
    let random = navigation_star_hash(cell);
    let point = 1.0 - smoothstep(0.015, 0.055,
        length(fract(coordinate) - random.xy));
    let temperature = navigation_star_hash(cell + vec2<f32>(19.0, 71.0));
    let color = mix(vec3<f32>(0.62, 0.75, 1.0),
        vec3<f32>(1.0, 0.78, 0.55), temperature.x);
    return color * point * step(threshold, random.z) *
        mix(0.4, 1.0, temperature.y);
}

fn navigation_space_background(uv : vec2<f32>) -> vec3<f32> {
    let ndc = uv * 2.0 - 1.0;
    let dimensions = vec2<f32>(uniforms.viewportBlend.xy);
    let aspect = dimensions.x / max(dimensions.y, 1.0);
    let cameraDirection = normalize(uniforms.cameraDirectionFocal.xyz);
    let cameraRight = normalize(uniforms.cameraRightStars.xyz);
    let cameraUp = normalize(uniforms.cameraUpControls.xyz);
    let focal = max(uniforms.cameraDirectionFocal.w, 0.001);
    let direction = normalize(-cameraDirection +
        cameraRight * (ndc.x * aspect / focal) -
        cameraUp * (ndc.y / focal));
    var radiance = (navigation_star_layer(direction, 512.0, 0.995) +
        navigation_star_layer(direction, 1024.0, 0.998) * 0.65) *
        max(uniforms.cameraRightStars.w, 0.0);
    let directionTowardSun = normalize(-uniforms.sunDirectionIntensity.xyz);
    if (uniforms.sunDisplay.y > 0.5 &&
        uniforms.sunDirectionIntensity.w > 0.0 &&
        dot(direction, directionTowardSun) >=
            cos(max(uniforms.sunColorAngularRadius.w, 0.0))) {
        radiance += uniforms.sunColorAngularRadius.xyz *
            uniforms.sunDirectionIntensity.w * uniforms.sunDisplay.x;
    }
    return radiance;
}

struct MergeFragmentOutput {
    @location(0) color : vec4<f32>,
    @builtin(frag_depth) depth : f32,
}

@fragment
fn drawNavigationMergeFragment(input : MergeVertexOutput) -> MergeFragmentOutput {
    let overviewPixel = scaledPixel(input.uv, textureDimensions(overviewColor));
    let rawOverview = textureLoad(overviewColor, overviewPixel, 0);
    let atmospherePixel = scaledPixel(
        input.uv, textureDimensions(overviewAtmosphere));
    let atmosphereOverview = textureLoad(
        overviewAtmosphere, atmospherePixel, 0);
    let overview = select(rawOverview, atmosphereOverview,
        atmosphereOverview.a > 0.001);
    let vertical = clamp(1.0 - input.uv.y * 2.0, -1.0, 1.0);
    let destination = select(
        uniforms.skyGround.rgb, uniforms.skyZenithExponent.rgb,
        vertical >= 0.0);
    var sky = mix(
        uniforms.skyHorizonIntensity.rgb, destination,
        pow(abs(vertical), max(uniforms.skyZenithExponent.w, 0.01))) *
        uniforms.skyHorizonIntensity.w;
    if (uniforms.cameraUpControls.w > 0.5) {
        sky = navigation_space_background(input.uv);
    }
    let overviewScene = mix(sky, overview.rgb, overview.a);
    let blend = clamp(uniforms.viewportBlend.z, 0.0, 1.0);
    var output : MergeFragmentOutput;
    if (blend >= 0.9999) {
        output.color = vec4<f32>(overviewScene, 1.0);
        output.depth = textureLoad(overviewDepth, overviewPixel, 0);
        return output;
    }
    let surfacePixel = scaledPixel(input.uv, textureDimensions(surfaceColor));
    let surface = textureLoad(surfaceColor, surfacePixel, 0);
    let surfaceZ = textureLoad(surfaceDepth, scaledPixel(
        input.uv, textureDimensions(surfaceDepth)), 0);
    if (blend <= 0.0001) {
        output.color = vec4<f32>(surface.rgb, 1.0);
        output.depth = surfaceZ;
        return output;
    }
    output.color = vec4<f32>(mix(surface.rgb, overviewScene, blend), 1.0);
    output.depth = select(
        surfaceZ, textureLoad(overviewDepth, overviewPixel, 0), blend >= 0.5);
    return output;
}
