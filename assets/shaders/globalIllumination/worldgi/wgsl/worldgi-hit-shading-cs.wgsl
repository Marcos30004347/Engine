#include "globalIllumination/worldgi/wgsl/worldgi-types.wgsl"

struct WorldGIHitShadingUniforms {
    viewport : vec2<u32>,
    rayGrid : vec2<u32>,
    raysPerPixel : u32,
    enabled : u32,
    diffuseEnabled : u32,
    glossyEnabled : u32,
    lightGridResolution : vec3<u32>,
    lightGridCascadeCount : u32,
    lightGridMaxItemsPerCell : u32,
    lightCount : u32,
    lightGridEnabled : u32,
    _paddingGrid0 : u32,
    lightGridCenterBaseExtent : vec4<f32>,
    lightGridCascadeScalePadding : vec4<f32>,
    clampValue : f32,
    diffuseScale : f32,
    glossyScale : f32,
    _padding0 : f32,
};

const PI : f32 = 3.14159265359;
const INV_PI : f32 = 0.31830988618;

@group(0) @binding(0) var<uniform> uniforms : WorldGIHitShadingUniforms;
@group(0) @binding(1) var<storage, read> worldGIRays : array<WorldGIRay>;
@group(0) @binding(2) var<storage, read> traceSurfaces : array<GITraceSurface>;
@group(0) @binding(3) var diffuseSeedTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(4) var glossySeedTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(5) var<storage, read> lights : array<WorldGILight>;
@group(0) @binding(6) var<storage, read> lightGridCells : array<WorldGILightGridCell>;
@group(0) @binding(7) var<storage, read> lightGridItemIds : array<u32>;

fn safe_normalize(value : vec3<f32>, fallback : vec3<f32>) -> vec3<f32> {
    let len = length(value);
    return select(fallback, value / len, len > 1.0e-6);
}

fn ray_is_valid(ray : WorldGIRay) -> bool {
    return (ray.typeBackendFlags.z & WORLD_GI_RAY_FLAG_VALID) != 0u &&
        ray.typeBackendFlags.x != WORLD_GI_RAY_TYPE_INVALID;
}

fn surface_is_valid(surface : GITraceSurface) -> bool {
    return (surface.sourceFlags.y & GI_TRACE_SURFACE_FLAG_VALID) != 0u &&
        surface.sourceFlags.x != GI_TRACE_SURFACE_SOURCE_INVALID &&
        surface.geometryNormalConfidence.w > 0.0 &&
        surface.worldPositionHitT.w >= 0.0;
}

fn light_is_enabled(light : WorldGILight) -> bool {
    return (light.flags.x & WORLD_GI_LIGHT_FLAG_ENABLED) != 0u &&
        light.flags.y != WORLD_GI_LIGHT_TYPE_INVALID &&
        light.colorIntensity.w > 0.0;
}

fn shade_light_at_hit(light : WorldGILight, position : vec3<f32>, normal : vec3<f32>, baseColor : vec3<f32>) -> vec3<f32> {
    if (!light_is_enabled(light)) {
        return vec3<f32>(0.0);
    }

    if (light.flags.y == WORLD_GI_LIGHT_TYPE_DIRECTIONAL) {
        let lightDirection = safe_normalize(-light.directionType.xyz, vec3<f32>(0.0, 1.0, 0.0));
        let nDotL = max(dot(normal, lightDirection), 0.0);
        let lightRadiance = max(light.colorIntensity.rgb, vec3<f32>(0.0)) * max(light.colorIntensity.w, 0.0);
        return baseColor * lightRadiance * (nDotL * INV_PI);
    }

    if (light.flags.y == WORLD_GI_LIGHT_TYPE_POINT && light.positionRange.w > 0.0) {
        let toLight = light.positionRange.xyz - position;
        let distanceSq = max(dot(toLight, toLight), 1.0e-4);
        let distance = sqrt(distanceSq);
        let lightDirection = toLight / distance;
        let range = max(light.positionRange.w, 1.0e-4);
        let rangeFade = clamp(1.0 - distance / range, 0.0, 1.0);
        let attenuation = rangeFade * rangeFade / distanceSq;
        let nDotL = max(dot(normal, lightDirection), 0.0);
        let lightRadiance = max(light.colorIntensity.rgb, vec3<f32>(0.0)) * max(light.colorIntensity.w, 0.0) * attenuation;
        return baseColor * lightRadiance * (nDotL * INV_PI);
    }

    if (light.flags.y == WORLD_GI_LIGHT_TYPE_SPHERE && light.positionRange.w > 0.0) {
        let toCenter = light.positionRange.xyz - position;
        let centerDistance = max(length(toCenter), 1.0e-4);
        let lightDirection = toCenter / centerDistance;
        let surfaceDistance = max(centerDistance - max(light.shapeParams.x, 0.0), 1.0e-4);
        let range = max(light.positionRange.w, 1.0e-4);
        let rangeFade = clamp(1.0 - surfaceDistance / range, 0.0, 1.0);
        let attenuation = rangeFade * rangeFade / max(surfaceDistance * surfaceDistance, 1.0e-4);
        let nDotL = max(dot(normal, lightDirection), 0.0);
        let lightRadiance = max(light.colorIntensity.rgb, vec3<f32>(0.0)) * max(light.colorIntensity.w, 0.0) * attenuation;
        return baseColor * lightRadiance * (nDotL * INV_PI);
    }

    if (light.flags.y == WORLD_GI_LIGHT_TYPE_PLANAR && light.positionRange.w > 0.0) {
        let lightPosition = worldgi_planar_light_closest_point(light, position);
        let toLight = lightPosition - position;
        let distanceSq = max(dot(toLight, toLight), 1.0e-4);
        let distance = sqrt(distanceSq);
        let lightDirection = toLight / distance;
        let basis = worldgi_planar_light_basis(light);
        let emissionTerm = max(dot(basis.normal, -lightDirection), 0.0);
        let range = max(light.positionRange.w, 1.0e-4);
        let rangeFade = clamp(1.0 - distance / range, 0.0, 1.0);
        let attenuation = rangeFade * rangeFade / distanceSq;
        let nDotL = max(dot(normal, lightDirection), 0.0);
        let lightRadiance = max(light.colorIntensity.rgb, vec3<f32>(0.0)) * max(light.colorIntensity.w, 0.0) * attenuation;
        return baseColor * lightRadiance * (nDotL * emissionTerm * INV_PI);
    }

    return vec3<f32>(0.0);
}

fn light_grid_cell_for_position(position : vec3<f32>) -> u32 {
    let resolution = max(uniforms.lightGridResolution.x, 1u);
    let cascadeCount = max(uniforms.lightGridCascadeCount, 1u);
    let cellsPerCascade = resolution * resolution * resolution;

    for (var cascadeIndex = 0u; cascadeIndex < cascadeCount; cascadeIndex = cascadeIndex + 1u) {
        let extent = max(uniforms.lightGridCenterBaseExtent.w, 1.0e-3) *
            pow(max(uniforms.lightGridCascadeScalePadding.x, 1.0), f32(cascadeIndex));
        let cascadeMin = uniforms.lightGridCenterBaseExtent.xyz - vec3<f32>(extent * 0.5);
        let local = position - cascadeMin;
        if (all(local >= vec3<f32>(0.0)) && all(local < vec3<f32>(extent))) {
            let cellSize = extent / f32(resolution);
            let coord = floor(local / cellSize);
            let maxCoord = f32(resolution - 1u);
            let cellX = u32(clamp(coord.x, 0.0, maxCoord));
            let cellY = u32(clamp(coord.y, 0.0, maxCoord));
            let cellZ = u32(clamp(coord.z, 0.0, maxCoord));
            return cascadeIndex * cellsPerCascade + cellZ * resolution * resolution + cellY * resolution + cellX;
        }
    }

    return 0xffffffffu;
}

fn directional_lighting_fallback(position : vec3<f32>, normal : vec3<f32>, baseColor : vec3<f32>) -> vec3<f32> {
    var radiance = vec3<f32>(0.0);
    let cappedLightCount = min(uniforms.lightCount, arrayLength(&lights));
    for (var lightIndex = 0u; lightIndex < cappedLightCount; lightIndex = lightIndex + 1u) {
        let light = lights[lightIndex];
        if (light.flags.y == WORLD_GI_LIGHT_TYPE_DIRECTIONAL) {
            radiance += shade_light_at_hit(light, position, normal, baseColor);
        }
    }
    return radiance;
}

fn direct_lighting_at_hit(surface : GITraceSurface) -> vec3<f32> {
    let normal = safe_normalize(surface.shadingNormalRoughness.xyz, surface.geometryNormalConfidence.xyz);
    let baseColor = clamp(surface.baseColorOpacity.rgb, vec3<f32>(0.0), vec3<f32>(8.0));
    let position = surface.worldPositionHitT.xyz;
    var radiance = max(surface.emissiveRadiance.rgb, vec3<f32>(0.0));

    if (uniforms.lightGridEnabled != 0u) {
        let cellIndex = light_grid_cell_for_position(position);
        if (cellIndex != 0xffffffffu && cellIndex < arrayLength(&lightGridCells)) {
            let cell = lightGridCells[cellIndex];
            let itemCount = min(cell.itemCount.x, uniforms.lightGridMaxItemsPerCell);
            let itemBase = cellIndex * max(uniforms.lightGridMaxItemsPerCell, 1u);
            for (var itemIndex = 0u; itemIndex < itemCount; itemIndex = itemIndex + 1u) {
                let itemAddress = itemBase + itemIndex;
                if (itemAddress >= arrayLength(&lightGridItemIds)) {
                    break;
                }
                let lightIndex = lightGridItemIds[itemAddress];
                if (lightIndex < min(uniforms.lightCount, arrayLength(&lights))) {
                    radiance += shade_light_at_hit(lights[lightIndex], position, normal, baseColor);
                }
            }
        } else {
            radiance += directional_lighting_fallback(position, normal, baseColor);
        }
    }

    return min(radiance, vec3<f32>(max(uniforms.clampValue, 0.0)));
}

fn diffuse_receiver_contribution(ray : WorldGIRay, hitRadiance : vec3<f32>) -> vec3<f32> {
    let receiverNormal = safe_normalize(ray.receiverNormalRoughness.xyz, vec3<f32>(0.0, 1.0, 0.0));
    let rayDirection = safe_normalize(ray.directionMaxT.xyz, receiverNormal);
    let receiverAlbedo = clamp(ray.receiverBaseColorMetallic.rgb, vec3<f32>(0.0), vec3<f32>(8.0));
    let cosTheta = max(dot(receiverNormal, rayDirection), 0.0);
    let rayPdf = max(ray.samplePdfMisWeight.x, 1.0e-6);
    let misWeight = clamp(ray.samplePdfMisWeight.w, 0.0, 1.0);
    return receiverAlbedo * hitRadiance * (cosTheta * INV_PI * misWeight / rayPdf);
}

fn glossy_receiver_contribution(ray : WorldGIRay, hitRadiance : vec3<f32>) -> vec3<f32> {
    let metallic = clamp(ray.receiverBaseColorMetallic.w, 0.0, 1.0);
    let specularTint = mix(vec3<f32>(0.04), clamp(ray.receiverBaseColorMetallic.rgb, vec3<f32>(0.0), vec3<f32>(1.0)), metallic);
    let rayPdf = max(ray.samplePdfMisWeight.x, 1.0e-6);
    let misWeight = clamp(ray.samplePdfMisWeight.w, 0.0, 1.0);
    return specularTint * hitRadiance * (misWeight / rayPdf);
}

fn tile_min(rayPixel : vec2<u32>) -> vec2<u32> {
    return vec2<u32>(
        (rayPixel.x * uniforms.viewport.x) / uniforms.rayGrid.x,
        (rayPixel.y * uniforms.viewport.y) / uniforms.rayGrid.y);
}

fn tile_max(rayPixel : vec2<u32>) -> vec2<u32> {
    return vec2<u32>(
        min(((rayPixel.x + 1u) * uniforms.viewport.x + uniforms.rayGrid.x - 1u) / uniforms.rayGrid.x, uniforms.viewport.x),
        min(((rayPixel.y + 1u) * uniforms.viewport.y + uniforms.rayGrid.y - 1u) / uniforms.rayGrid.y, uniforms.viewport.y));
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x >= uniforms.rayGrid.x || gid.y >= uniforms.rayGrid.y) {
        return;
    }

    let rayPixel = gid.xy;
    let rayPixelIndex = rayPixel.y * uniforms.rayGrid.x + rayPixel.x;
    let baseRayIndex = rayPixelIndex * uniforms.raysPerPixel;
    let rayCount = min(arrayLength(&worldGIRays), arrayLength(&traceSurfaces));

    var diffuse = vec3<f32>(0.0);
    var glossy = vec3<f32>(0.0);
    var diffuseWeight = 0.0;
    var glossyWeight = 0.0;

    if (uniforms.enabled != 0u) {
        for (var sampleIndex = 0u; sampleIndex < uniforms.raysPerPixel; sampleIndex = sampleIndex + 1u) {
            let rayIndex = baseRayIndex + sampleIndex;
            if (rayIndex >= rayCount) {
                continue;
            }

            let ray = worldGIRays[rayIndex];
            let surface = traceSurfaces[rayIndex];
            if (!ray_is_valid(ray) || !surface_is_valid(surface)) {
                continue;
            }

            let hitRadiance = direct_lighting_at_hit(surface);
            if (ray.typeBackendFlags.x == WORLD_GI_RAY_TYPE_DIFFUSE && uniforms.diffuseEnabled != 0u) {
                diffuse += diffuse_receiver_contribution(ray, hitRadiance);
                diffuseWeight += 1.0;
            } else if (ray.typeBackendFlags.x == WORLD_GI_RAY_TYPE_GLOSSY && uniforms.glossyEnabled != 0u) {
                glossy += glossy_receiver_contribution(ray, hitRadiance);
                glossyWeight += 1.0;
            }
        }
    }

    let diffuseConfidence = min(diffuseWeight / max(f32(uniforms.raysPerPixel), 1.0), 1.0);
    let glossyConfidence = min(glossyWeight / max(f32(uniforms.raysPerPixel), 1.0), 1.0);
    let diffuseOut = vec4<f32>(min(diffuse * uniforms.diffuseScale, vec3<f32>(uniforms.clampValue)), diffuseConfidence);
    let glossyOut = vec4<f32>(min(glossy * uniforms.glossyScale, vec3<f32>(uniforms.clampValue)), glossyConfidence);
    let begin = tile_min(rayPixel);
    let end = tile_max(rayPixel);
    for (var y = begin.y; y < end.y; y = y + 1u) {
        for (var x = begin.x; x < end.x; x = x + 1u) {
            let pixel = vec2<i32>(i32(x), i32(y));
            textureStore(diffuseSeedTexture, pixel, diffuseOut);
            textureStore(glossySeedTexture, pixel, glossyOut);
        }
    }
}
