const LAMBERTIAN_PI : f32 = 3.141592653589793;
const LAMBERTIAN_INV_PI : f32 = 1.0 / LAMBERTIAN_PI;

fn lambertian_diffuse_brdf(albedo : vec3<f32>) -> vec3<f32> {
    return max(albedo, vec3<f32>(0.0)) * LAMBERTIAN_INV_PI;
}

fn evaluate_lambertian_diffuse(
    albedo : vec3<f32>,
    n : vec3<f32>,
    wi : vec3<f32>) -> vec3<f32> {
    return lambertian_diffuse_brdf(albedo) * max(dot(n, wi), 0.0);
}

