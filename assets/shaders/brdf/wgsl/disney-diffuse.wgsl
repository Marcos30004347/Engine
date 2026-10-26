const DISNEY_DIFFUSE_PI : f32 = 3.141592653589793;
const DISNEY_DIFFUSE_INV_PI : f32 = 1.0 / DISNEY_DIFFUSE_PI;

fn disney_fresnel_weight(cosTheta : f32) -> f32 {
    let m = clamp(1.0 - cosTheta, 0.0, 1.0);
    let m2 = m * m;
    return m2 * m2 * m;
}

fn disney_diffuse_brdf(
    albedo : vec3<f32>,
    roughness : f32,
    n : vec3<f32>,
    wi : vec3<f32>,
    wo : vec3<f32>) -> vec3<f32> {
    let h = normalize(wi + wo);
    let noL = max(dot(n, wi), 0.0);
    let noV = max(dot(n, wo), 0.0);
    let loH = max(dot(wi, h), 0.0);
    let fd90 = 0.5 + 2.0 * loH * loH * clamp(roughness, 0.0, 1.0);
    let lightScatter = 1.0 + (fd90 - 1.0) * disney_fresnel_weight(noL);
    let viewScatter = 1.0 + (fd90 - 1.0) * disney_fresnel_weight(noV);
    return max(albedo, vec3<f32>(0.0)) * DISNEY_DIFFUSE_INV_PI * lightScatter * viewScatter;
}

