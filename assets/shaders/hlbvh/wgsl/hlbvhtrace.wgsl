const STACK_SIZE = 64u;
const U32_MAX = 4294967295u;

struct Triangle {
    vertice0: u32,
    vertice1: u32,
    vertice2: u32,
    normal0: u32,
    normal1: u32,
    normal2: u32,
    uv0: u32,
    uv1: u32,
    uv2: u32,
};

struct HLBVHNode {
    parent: u32,
    left: u32,
    right: u32,
    next: u32,
    aabbMinX: f32,
    aabbMinY: f32,
    aabbMinZ: f32,
    aabbMaxX: f32,
    aabbMaxY: f32,
    aabbMaxZ: f32,
};

struct RayIntersection {
    primitiveId: u32,
    uvwt: vec4<f32>,
};


struct Camera {
    fov: f32,
    aspectRatio: f32,
    width: u32,
    height: u32,

    position: vec3<f32>,
    direction: vec3<f32>,
    cameraUp: vec3<f32>,
    cameraRight: vec3<f32>,

}

@group(0) @binding(0) var<uniform> camera: Camera;
@group(0) @binding(1) var<storage, read_write> colorBuffer: array<vec4<f32>>;
@group(0) @binding(2) var<storage, read> nodes: array<HLBVHNode>;
@group(0) @binding(3) var<storage, read> triangles: array<Triangle>;
@group(0) @binding(4) var<storage, read> vertices: array<vec3<f32>>;
@group(0) @binding(5) var<storage, read> normals: array<vec3<f32>>;
@group(0) @binding(6) var<storage, read> uvs: array<vec2<f32>>;

const PI: f32 = 3.14159265359;


fn generate_ray(pixel_coords: vec2<f32>, resolution: vec2<f32>) -> vec3<f32> {
    // Normalized Device Coordinates (NDC)
    let ndc_x = (pixel_coords.x + 0.5) / resolution.x;
    let ndc_y = (pixel_coords.y + 0.5) / resolution.y;

    // Screen space coordinates
    let screen_x = (2.0 * ndc_x - 1.0) * camera.aspectRatio * tan(camera.fov / 2.0);
    let screen_y = (1.0 - 2.0 * ndc_y) * tan(camera.fov / 2.0);

    // Calculate ray direction
    let ray_dir = normalize(
        camera.direction +
        camera.cameraRight * screen_x +
        camera.cameraUp * screen_y
    );

    return ray_dir;
}


fn intersect_ray_aabb(ray_origin: vec3<f32>, ray_dir: vec3<f32>, aabb_min: vec3<f32>, aabb_max: vec3<f32>) -> f32 {
    // Calculate inverse ray direction
    let inv_dir = 1.0 / ray_dir;

    // Calculate intersection distances for each axis
    let t1 = (aabb_min - ray_origin) * inv_dir;
    let t2 = (aabb_max - ray_origin) * inv_dir;

    // Compute min and max t-values for intersections
    let tmin = max(max(min(t1.x, t2.x), min(t1.y, t2.y)), min(t1.z, t2.z));
    let tmax = min(min(max(t1.x, t2.x), max(t1.y, t2.y)), max(t1.z, t2.z));

    // Check if the ray intersects the AABB
    if tmax < max(tmin, 0.0) {
        return -1.0; // No intersection
    }

    return tmin; // Return the nearest intersection distance
}

fn fast_intersect_bbox1(boxmin: vec3<f32>, boxmax: vec3<f32>, invdir: vec3<f32>, oxinvdir: vec3<f32>, t_max: f32) -> vec2<f32> {
    let f = boxmax * invdir + oxinvdir;
    let n = boxmin * invdir + oxinvdir;

    let tmax = max(f, n); // Component-wise maximum
    let tmin = min(f, n); // Component-wise minimum

    let t1 = min(min(min(tmax.x, tmax.y), tmax.z), t_max); // Minimum of tmax and t_max
    let t0 = max(max(max(tmin.x, tmin.y), tmin.z), 0.0);   // Maximum of tmin and 0.0

    return vec2<f32>(t0, t1);
}

fn intersect_bvh_leaf_triangle(ray_origin: vec3<f32>, ray_dir: vec3<f32>, primitiveId: u32, isect: ptr<function, RayIntersection>) -> bool {
    let tri = triangles[primitiveId];
    
    let v1 = vertices[tri.vertice0];
    let v2 = vertices[tri.vertice1];
    let v3 = vertices[tri.vertice2];

    let e1 = v2 - v1;
    let e2 = v3 - v1;
    let h = cross(ray_dir, e2);
    let a = dot(e1, h);
    
    if a > -0.0001 && a < 0.0001 {
        return false;
    }

    let f = 1.0 / a;
    let s = ray_origin - v1;
    let u = dot(s, h) * f;
    let q = cross(s, e1);
    let v = dot(ray_dir, q) * f;
    let t = dot(e2, q) * f;

    if (u < 0.0 || u > 1.0 || v < 0.0 || u + v > 1.0 || t < 0.0 ) {
        return false;
    } else {
        (*isect).primitiveId = primitiveId;
        (*isect).uvwt = vec4<f32>(u, v, t, 0.0);
        return true;
    }
}

fn is_leaf_node(node: HLBVHNode) -> bool {
    return node.left == node.right;
}

fn intersect_scene(ray_origin: vec3<f32>, ray_dir: vec3<f32>, intersection: ptr<function, RayIntersection>) -> bool {
    let invdir = 1.0 / ray_dir;
    let oxinvdir = -ray_origin * invdir;

    var stack: array<u32, STACK_SIZE>;

    var top: u32 = 0;
    stack[top] = U32_MAX;

    top += 1u;

    var idx: u32 = 0u;

    var lefthit: f32 = 0.0;
    var righthit: f32 = 0.0;

    var hit: bool = false;
    
    var depth = 0.0;
    while (idx != U32_MAX) {
        let node = nodes[idx];
        
        depth += 1.0;

        if is_leaf_node(node) {
            intersection.uvwt.a = depth;
            if (intersect_bvh_leaf_triangle(ray_origin, ray_dir, node.left, intersection)) {
                intersection.uvwt.a = depth;
                return true;
            }
        } else {
            let lbox = nodes[node.left];
            let rbox = nodes[node.right];

            lefthit = intersect_ray_aabb(ray_origin, ray_dir, vec3<f32>(lbox.aabbMinX, lbox.aabbMinY, lbox.aabbMinZ), vec3<f32>(lbox.aabbMaxX, lbox.aabbMaxY, lbox.aabbMaxZ));
            righthit = intersect_ray_aabb(ray_origin, ray_dir, vec3<f32>(rbox.aabbMinX, rbox.aabbMinY, rbox.aabbMinZ), vec3<f32>(rbox.aabbMaxX, rbox.aabbMaxY, rbox.aabbMaxZ));

            if (lefthit > 0.0 && righthit > 0.0) {
                var deferred: u32 = U32_MAX;
                
                idx = select(node.left, node.right, lefthit > righthit);
                stack[top] = select(node.right, node.left, lefthit > righthit);
                top += 1;
               // return true;
                continue;
            } else if (lefthit > 0.0) {
                idx = node.left;
                continue;
            } else if (righthit > 0.0) {
                idx = node.right;
                continue;
            }
        }

        top -= 1u;
        idx = stack[top];
    }

    return hit;
}

fn intersect_sphere(rayOrigin: vec3<f32>, rayDir: vec3<f32>, sphereCenter: vec3<f32>, sphereRadius: f32) -> f32 {
    let oc = rayOrigin - sphereCenter;
    let a = dot(rayDir, rayDir);
    let b = 2.0 * dot(oc, rayDir);
    let c = dot(oc, oc) - sphereRadius * sphereRadius;

    let discriminant = b * b - 4.0 * a * c;
    
    var hitDepth = 0.0f;

    if discriminant > 0.0 {
        let t1 = (-b - sqrt(discriminant)) / (2.0 * a);
        let t2 = (-b + sqrt(discriminant)) / (2.0 * a);

        // Store the nearest positive depth
        if t1 > 0.0 && t2 > 0.0 {
            hitDepth = min(t1, t2);
        } else if t1 > 0.0 {
            hitDepth = t1;
        } else if t2 > 0.0 {
            hitDepth = t2;
        } else {
            return -1.0; // Both intersections are behind the ray origin
        }

        return hitDepth; // Ray intersects the sphere
    }

    return -1.0; // No intersection
}

@compute @workgroup_size(16, 16, 1)
fn shot_ray_from_camera(@builtin(global_invocation_id) id: vec3<u32>) {
    let x = f32(id.x);
    let y = f32(id.y);
    
    let rayDir = generate_ray(vec2<f32>(x, y), vec2<f32>(f32(camera.width), f32(camera.height)));
    let rayOrigin = camera.position;

    var rayIntersection: RayIntersection;

    var color = vec4<f32>(0,0,0,0); 
   // var depth = intersect_sphere(rayOrigin, rayDir, vec3<f32>(-0.016841,0.110154,-0.001537), 0.2);

   // if depth > 0 {
    //    color = vec4<f32>(depth,depth,depth,1); 
    //}

    if intersect_scene(rayOrigin, rayDir, &rayIntersection) {
        rayIntersection.uvwt.a /= 100.0;
        color = vec4<f32>(rayIntersection.uvwt.a,rayIntersection.uvwt.a,rayIntersection.uvwt.a,1); 
    }

    colorBuffer[id.y * camera.width + id.x] = color;
}

