#include "sparse_voxel_types.wgsl"
#include "globalIllumination/worldgi/wgsl/worldgi-types.wgsl"

// Draws the finite emitters into the composed image.
//
// An area light that illuminates a room but is itself invisible is very hard to
// author against: there is nothing on screen to tell you where it is, which way
// a quad faces, or how large it is, so placing one is guesswork checked against
// the shading it produces several frames later.
//
// The emitters are drawn analytically rather than as meshes. There are at most
// a handful of them and each is a sphere or a quad, so intersecting the camera
// ray with all of them costs a few dozen instructions per pixel and needs no
// vertex buffers, no draw calls and no geometry pipeline - none of which this
// renderer otherwise has. Depth comes from the same linear depth buffer the
// lighting passes read, so emitters occlude and are occluded correctly without
// a depth attachment.
//
// This is a visualisation, not a light transport path: the emitter is drawn at
// its own radiance and contributes nothing to the shading, which already
// sampled it through the light record. Drawing it does not double count.

struct SparseVoxelLightGizmoSettings {
    // xy raster dimensions, z light count, w selected light handle
    // (0xffffffff for none).
    dimensionsLights : vec4<u32>,
    // x emitter draw enable, y selection outline width in pixels, z unused,
    // w unused.
    options : vec4<f32>,
}

@group(0) @binding(0) var<uniform> raster : SparseVoxelRasterUniforms;
@group(0) @binding(1) var<uniform> settings : SparseVoxelLightGizmoSettings;
@group(0) @binding(2) var<storage, read> lights : array<WorldGILight>;
@group(0) @binding(3) var<storage, read> coverage : array<u32>;
@group(0) @binding(4) var<storage, read> linearDepth : array<f32>;
@group(0) @binding(5) var output : texture_storage_2d<rgba16float, read_write>;

// Nearest positive root of the ray/sphere quadratic, or -1 when the ray misses.
fn gizmo_intersect_sphere(
    origin : vec3<f32>, direction : vec3<f32>, centre : vec3<f32>,
    radius : f32) -> f32 {
    let offset = origin - centre;
    let b = dot(offset, direction);
    let c = dot(offset, offset) - radius * radius;
    let discriminant = b * b - c;
    if (discriminant < 0.0) { return -1.0; }
    let root = sqrt(discriminant);
    let near = -b - root;
    if (near > 0.0) { return near; }
    let far = -b + root;
    return select(-1.0, far, far > 0.0);
}

// Distance to the quad's plane if the ray lands inside the emitter's extents,
// otherwise -1. `edge` comes back as the smallest distance to a border in
// units of the half extents, which the outline below thresholds against.
struct GizmoQuadHit {
    distance : f32,
    edge : f32,
}

fn gizmo_intersect_quad(
    origin : vec3<f32>, direction : vec3<f32>, light : WorldGILight)
    -> GizmoQuadHit {
    let miss = GizmoQuadHit(-1.0, 0.0);
    let basis = worldgi_planar_light_basis(light);
    let normal = normalize(light.directionType.xyz);
    let denominator = dot(direction, normal);
    // Emitting quads are single sided in the estimator but must be visible from
    // both sides here: a light drawn only from its front is invisible from
    // exactly the position you stand in to check which way it faces.
    if (abs(denominator) < 1.0e-6) { return miss; }
    let travel = dot(light.positionRange.xyz - origin, normal) / denominator;
    if (travel <= 0.0) { return miss; }
    let point = origin + direction * travel - light.positionRange.xyz;
    // Half extents live in shapeParams.yz; slot x is the sphere radius and is
    // zero for a quad, so reading .xy collapses the emitter to a hairline that
    // is invisible at any size.
    let halfExtent = max(light.shapeParams.yz, vec2<f32>(1.0e-4));
    let local = vec2<f32>(
        dot(point, basis.tangent), dot(point, basis.bitangent)) / halfExtent;
    if (any(abs(local) > vec2<f32>(1.0))) { return miss; }
    return GizmoQuadHit(travel, 1.0 - max(abs(local.x), abs(local.y)));
}

@compute @workgroup_size(8, 8, 1)
fn drawSparseVoxelLightGizmos(@builtin(global_invocation_id) gid : vec3<u32>) {
    let dimensions = max(settings.dimensionsLights.xy, vec2<u32>(1u));
    if (any(gid.xy >= dimensions)) { return; }
    if (settings.options.x == 0.0) { return; }
    let pixelIndex = gid.y * dimensions.x + gid.x;

    // Camera ray for this pixel, built the same way the probe and per-pixel
    // paths build theirs so the emitter lands exactly where it lights.
    let cameraDirection = vec3<f32>(
        (f32(gid.x) + 0.5 - raster.focalPrincipal.z) / raster.focalPrincipal.x,
        (f32(gid.y) + 0.5 - raster.focalPrincipal.w) / raster.focalPrincipal.y,
        1.0);
    let direction = normalize(
        (raster.cameraToWorld * vec4<f32>(cameraDirection, 0.0)).xyz);
    let origin = raster.cameraWorldPositionNear.xyz;

    // How far the ray may travel before the scene occludes it. An uncovered
    // pixel is open sky, so an emitter behind it is still visible.
    var sceneDistance = 3.402823466e+38f;
    if (pixelIndex < arrayLength(&coverage) &&
        pixelIndex < arrayLength(&linearDepth) &&
        coverage[pixelIndex] != 0u) {
        // linearDepth is already a distance along this pixel's *normalised*
        // world ray, not an axial view depth - the resolve stores the ray
        // parameter and probe_world_position reconstructs with
        // `origin + normalize(direction) * depth`. Dividing by the axial
        // cosine, as an axial depth would need, inflates the occluder distance
        // by up to a third at the screen edges, and an emitter inside that
        // margin behind a wall draws straight through it.
        sceneDistance = linearDepth[pixelIndex];
    }

    let lightCount = min(settings.dimensionsLights.z, arrayLength(&lights));
    var nearest = sceneDistance;
    var emission = vec3<f32>(0.0);
    var hitAny = false;
    var selected = false;
    var edge = 1.0;
    for (var index = 0u; index < lightCount; index += 1u) {
        let light = lights[index];
        if ((light.flags.x & WORLD_GI_LIGHT_FLAG_ENABLED) == 0u) { continue; }
        var travel = -1.0;
        var localEdge = 1.0;
        if (light.flags.y == WORLD_GI_LIGHT_TYPE_SPHERE) {
            travel = gizmo_intersect_sphere(
                origin, direction, light.positionRange.xyz,
                max(light.shapeParams.x, 1.0e-3));
        } else if (light.flags.y == WORLD_GI_LIGHT_TYPE_PLANAR) {
            let hit = gizmo_intersect_quad(origin, direction, light);
            travel = hit.distance;
            localEdge = hit.edge;
        } else if (light.flags.y == WORLD_GI_LIGHT_TYPE_POINT) {
            // A point light has no area to draw. Give it a small screen-sized
            // marker so it can still be seen and picked: a fixed world radius
            // would vanish at distance and swallow the scene up close.
            let toLight = light.positionRange.xyz - origin;
            let along = dot(toLight, direction);
            if (along > 0.0) {
                travel = gizmo_intersect_sphere(
                    origin, direction, light.positionRange.xyz,
                    along * 0.004);
            }
        }
        if (travel <= 0.0 || travel >= nearest) { continue; }
        nearest = travel;
        // The emitter is drawn at the radiance it emits, so a brighter light
        // reads as brighter on screen and the exposure that suits the scene
        // suits the emitter too.
        emission = light.colorIntensity.xyz * light.colorIntensity.w;
        hitAny = true;
        edge = localEdge;
        selected = light.flags.z == settings.dimensionsLights.w;
    }
    if (!hitAny) { return; }

    var colour = emission;
    // Outline the selected emitter so the list selection and the thing in the
    // viewport are unambiguously the same light.
    if (selected && edge < max(settings.options.y, 0.0)) {
        colour = mix(colour, vec3<f32>(1.0, 0.6, 0.1), 0.85);
    }
    let previous = textureLoad(output, vec2<i32>(gid.xy));
    textureStore(output, vec2<i32>(gid.xy), vec4<f32>(colour, previous.w));
}
