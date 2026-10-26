#include "brdf/wgsl/lambertian-diffuse.wgsl"

const DIFFUSE_BRDF_LAMBERTIAN : u32 = 0u;
const DIFFUSE_BRDF_EON : u32 = 1u;
const DIFFUSE_BRDF_DISNEY : u32 = 2u;

fn evaluate_diffuse_brdf(
    model : u32,
    albedo : vec3<f32>,
    roughness : f32,
    n : vec3<f32>,
    wi : vec3<f32>,
    wo : vec3<f32>) -> vec3<f32> {
    _ = roughness;
    _ = wo;
    switch model {
        default: {
            return lambertian_diffuse_brdf(albedo) * max(dot(n, wi), 0.0);
        }
    }
}

