const U32_MAX = 4294967295u;

struct Uniforms {
    num: u32,
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

struct RayIntersection {
    primitiveId: u32,
    uvwt: vec4<f32>,
};

@group(0) @binding(0) var<uniform> uniforms : Uniforms;
@group(0) @binding(1) var<uniform> camera : Camera;
@group(0) @binding(2) var<storage, read> nodes : array<vec4<u32>>;
@group(0) @binding(3) var<storage, read_write> colorBuffer: array<vec4<f32>>;
@group(0) @binding(4) var<storage, read> triangles: array<vec4<f32>>;

const PI: f32 = 3.14159265359;


fn rand(seed: vec2<f32>) -> f32 {
    let a: f32 = 12.9898;
    let b: f32 = 78.233;
    let c: f32 = 43758.5453;
    let dt: f32 = dot(seed, vec2<f32>(a, b));
    let sn: f32 = fract(sin(dt) * c);
    return sn;
}

fn randomColor(seed: f32) -> vec4<f32> {
    let r = rand(vec2<f32>(seed, 0.1));
    let g = rand(vec2<f32>(seed, 0.2));
    let b = rand(vec2<f32>(seed, 0.3));
    let a = 1.0; 
    return vec4<f32>(r, g, b, a);
}

fn generate_ray(pixel_coords: vec2<f32>, resolution: vec2<f32>) -> vec3<f32> {
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

fn extract_byte(i : u32, n: u32) -> u32 {
     return (i >> (n * 8u)) & 0xFFu; 
}

// fn set_byte(i: u32, n: u32, byte: u32) -> u32 {
//     let mask = !(0xFF << (n * 8)); 
//     return (i & mask) | ((byte & 0xFF) << (n * 8));
// }


// TODO: implement shared stack

// fn sign_extend_s8x4(i: u32) -> u32 {
//     let b0 = select(0u, 0xff000000u, (i & 0x80000000u) != 0u);
//     let b1 = select(0u, 0x00ff0000u, (i & 0x00008000u) != 0u);
//     let b2 = select(0u, 0x0000ff00u, (i & 0x00000080u) != 0u);
//     let b3 = select(0u, 0x000000ffu, (i & 0x00000008u) != 0u);
//     return b0 + b1 + b2 + b3;
// }
fn sign_extend_s8x4(x: u32) -> u32
{
	return ((x >> 7u) & 0x01010101u) * 0xffu;
}

fn intersect_bvh_leaf_triangle(ray_origin: vec3<f32>, ray_dir: vec3<f32>, v1: vec3<f32>, v2: vec3<f32>, v3: vec3<f32> , isect: ptr<function, RayIntersection>) -> bool {
   // let tri = triangles[primitiveId];
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
        //(*isect).primitiveId = primitiveId;
        (*isect).uvwt = vec4<f32>(t, t, t, t);
        return true;
    }
}

// fn intersect_bvh_leaf_triangle(ray_origin: vec3<f32>, ray_dir: vec3<f32>, v0: vec3<f32>, v11: vec3<f32>, v22: vec3<f32> , isect: ptr<function, RayIntersection>) -> bool {
//    // let tri = triangles[primitiveId];
// 	let v1 = v11 - v0;
// 	let v2 = v22 - v0;

// 	let pv = cross(ray_dir, v2);
// 	//float det = dot(e0, pv);

// 	let tv = ray_origin - v0;
// 	let qv = cross(tv, v1);

// 	var uvt = vec4<f32>();
// 	uvt.x = dot(tv, pv);//u
// 	//if(uvt.x < 0.0f)
// 	//	return false;

// 	uvt.y = dot(ray_dir, qv);//v
// 	//if(uvt.y < 0.0f)
// 	//	return false;

// 	uvt.z = dot(v2, qv);//t
// 	let inv_det = 1.0 / dot(v1, pv);//1.0f / det;

// 	uvt.x = uvt.x * inv_det;
// 	uvt.y = uvt.y * inv_det;
// 	uvt.z = uvt.z * inv_det;
// 	uvt.w = 1.0 - uvt.x - uvt.y;//u + v >= det

// 	return uvt.x > 0.0 && uvt.y > 0.0 && uvt.z > 0.0 && uvt.w > 0.0;
// 	// return (uvt.z < max_t && all(greaterThanEqual(uvt, vec4(0.0f))));
// }


fn get_oct_inv4(d: vec3<f32>) -> u32
{
	return select(0x04040404u, 0u, d.x < 0.0) |
		   select(0x02020202u, 0u, d.y < 0.0) |
		   select(0x01010101u, 0u, d.z < 0.0);
}

fn trace(origin_tmin : vec4<f32>, dir_tmax: vec4<f32>, hit: ptr<function, RayIntersection>) {
	let orig = vec3<f32>(origin_tmin.xyz);
	let dir = vec3<f32>(dir_tmax.xyz);

    var tmin = origin_tmin.w; // 0.01f
	var tmax = dir_tmax.w; // 1e20

    var traversalStack: array<vec2<u32>, 32>;
    var stackPtr: i32 = 0;

    let invdir: vec3<f32> = 1.0 / dir;

    // Calculate octant inversion
    var octinv = ((select(0u, 4u, dir.x < 0.0)) | (select(0u, 2u, dir.y < 0.0)) | (select(0u, 1u, dir.z < 0.0)));// * 0x1010101;
    octinv = 7u - octinv;
    //let octinv4 = get_oct_inv4(dir);

    var retVal: bool = false;
    var voxCent: vec3<f32> = vec3<f32>(0.0, 0.0, 0.0);
    var outNorm: vec3<f32> = vec3<f32>(0.0, 0.0, 0.0);
    var packedMat: vec4<f32> = vec4<f32>(0.0, 0.0, 0.0, 0.0);

    var ngroup = vec2<u32>(0u, 0x80000000u);
    var tgroup = vec2<u32>(0u,0u);
    
	var triangleuv = vec2<f32>();
    var depth = 0;
    var maxDepth = 4;
    var hitAddr = U32_MAX;
    
    var intersections = 0;
    var outcolor = vec4<f32>(0.0,0.0,0.0,0.0);
    
    loop {
        if ngroup.y > 0x00FFFFFFu { // maybe use & for speed 0xff000000
            var hits = ngroup.y;
            var imask = ngroup.y;
			var child_bit_index = firstLeadingBit(hits);
			var child_node_base_index = ngroup.x;

			ngroup.y &= ~(1u << child_bit_index);

            if ngroup.y > 0x00FFFFFFu {
                traversalStack[stackPtr] = ngroup;
                stackPtr += 1;
            
                if stackPtr >= 32 {
                    hit.uvwt = vec4<f32>(f32(10000), 0.0, 0.0, 1.0);
                }
            }

            {
                let slot_index: u32 = (child_bit_index - 24u) ^ (octinv);
                let octinv4 = octinv * 0x01010101u;
                let relative_index: u32 = countOneBits(imask & ~(0xFFFFFFFFu << slot_index));
                let child_node_index: u32 = child_node_base_index + relative_index;

                // Load the 80 bytes of the CWBVH
                let n0: vec4<u32> = nodes[child_node_index * 5 + 0];
                let n1: vec4<u32> = nodes[child_node_index * 5 + 1];
                let n2: vec4<u32> = nodes[child_node_index * 5 + 2];
                let n3: vec4<u32> = nodes[child_node_index * 5 + 3];
                let n4: vec4<u32> = nodes[child_node_index * 5 + 4];

                let p: vec3<f32> = vec3<f32>(
                    bitcast<f32>(n0.x),
                    bitcast<f32>(n0.y),
                    bitcast<f32>(n0.z)
                );

                let e_imask = unpack4xU8(n0.w);

                ngroup.x = n1.x; // ntbi[0]
                
                tgroup.x = n1.y; // ntbi[1]
                tgroup.y = 0u;

                var hitmask: u32 = 0u;

                let adjusted_idirx: f32 = bitcast<f32>((e_imask.x + 127) << 23u) * invdir.x;
                let adjusted_idiry: f32 = bitcast<f32>((e_imask.y + 127) << 23u) * invdir.y;
                let adjusted_idirz: f32 = bitcast<f32>((e_imask.z + 127) << 23u) * invdir.z;

                let origx: f32 = -(orig.x - p.x) * invdir.x;
                let origy: f32 = -(orig.y - p.y) * invdir.y;
                let origz: f32 = -(orig.z - p.z) * invdir.z;

                {
                    // First 4
                    let meta4: u32 = n1.z; // n1.z maps to meta as [8b child 0] [8b child 1] [8b child 2] [8b child 3]
                    let is_inner4: u32 = (meta4 & (meta4 << 1)) & 0x10101010;
                    let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3);// (is_inner4 << 3) as i32 >> 31;
                    let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                    let child_bits4: u32 = (meta4 >> 5) & 0x07070707;

                    // n2.x == qlox[0]
                    // n3.z == qhix[0]

                    // n2.z == qloy[0]
                    // n4.x == qhiy[0]
    
                    // n3.x == qloz[0]
                    // n4.z == qhiz[0]
                    let swizzledLox = select(n2.x, n3.z, invdir.x < 0.0);
                    let swizzledHix = select(n3.z, n2.x, invdir.x < 0.0);

                    let swizzledLoy = select(n2.z, n4.x, invdir.y < 0.0);
                    let swizzledHiy = select(n4.x, n2.z, invdir.y < 0.0);

                    let swizzledLoz = select(n3.x, n4.z, invdir.z < 0.0);
                    let swizzledHiz = select(n4.z, n3.x, invdir.z < 0.0);

                    var tminx = vec4<f32>();
                    var tminy = vec4<f32>();
                    var tminz = vec4<f32>();
                    var tmaxx = vec4<f32>();
                    var tmaxy = vec4<f32>();
                    var tmaxz = vec4<f32>();

                    tminx[0] = f32((swizzledLox >>  0) & 0xFF) * adjusted_idirx + origx;
                    tminx[1] = f32((swizzledLox >>  8) & 0xFF) * adjusted_idirx + origx;
                    tminx[2] = f32((swizzledLox >> 16) & 0xFF) * adjusted_idirx + origx;
                    tminx[3] = f32((swizzledLox >> 24) & 0xFF) * adjusted_idirx + origx;

                    tminy[0] = f32((swizzledLoy >>  0) & 0xFF) * adjusted_idiry + origy;
                    tminy[1] = f32((swizzledLoy >>  8) & 0xFF) * adjusted_idiry + origy;
                    tminy[2] = f32((swizzledLoy >> 16) & 0xFF) * adjusted_idiry + origy;
                    tminy[3] = f32((swizzledLoy >> 24) & 0xFF) * adjusted_idiry + origy;

                    tminz[0] = f32((swizzledLoz >>  0) & 0xFF) * adjusted_idirz + origz;
                    tminz[1] = f32((swizzledLoz >>  8) & 0xFF) * adjusted_idirz + origz;
                    tminz[2] = f32((swizzledLoz >> 16) & 0xFF) * adjusted_idirz + origz;
                    tminz[3] = f32((swizzledLoz >> 24) & 0xFF) * adjusted_idirz + origz;

                    tmaxx[0] = f32((swizzledHix >>  0) & 0xFF) * adjusted_idirx + origx;
                    tmaxx[1] = f32((swizzledHix >>  8) & 0xFF) * adjusted_idirx + origx;
                    tmaxx[2] = f32((swizzledHix >> 16) & 0xFF) * adjusted_idirx + origx;
                    tmaxx[3] = f32((swizzledHix >> 24) & 0xFF) * adjusted_idirx + origx;

                    tmaxy[0] = f32((swizzledHiy >>  0) & 0xFF) * adjusted_idiry + origy;
                    tmaxy[1] = f32((swizzledHiy >>  8) & 0xFF) * adjusted_idiry + origy;
                    tmaxy[2] = f32((swizzledHiy >> 16) & 0xFF) * adjusted_idiry + origy;
                    tmaxy[3] = f32((swizzledHiy >> 24) & 0xFF) * adjusted_idiry + origy;

                    tmaxz[0] = f32((swizzledHiz >>  0) & 0xFF) * adjusted_idirz + origz;
                    tmaxz[1] = f32((swizzledHiz >>  8) & 0xFF) * adjusted_idirz + origz;
                    tmaxz[2] = f32((swizzledHiz >> 16) & 0xFF) * adjusted_idirz + origz;
                    tmaxz[3] = f32((swizzledHiz >> 24) & 0xFF) * adjusted_idirz + origz;

                    for (var i = 0u; i < 4u; i++)
                    {
                        // Use VMIN, VMAX to compute the slabs
                        let cmin = max(max(max(tminx[i], tminy[i]), tminz[i]), tmin);
                        let cmax = min(min(min(tmaxx[i], tmaxy[i]), tmaxz[i]), tmax);
                        
                        let intersected = cmin <= cmax;
                        if  intersected {
                           // intersections += 1;
                            hitmask |= extract_byte(child_bits4, i) << extract_byte(bit_index4, i);
                        }
                    }
                }
                {
                    // Second 4
                    let meta4: u32 = n1.w; // n1.z maps to meta as [8b child 4] [8b child 5] [8b child 6] [8b child 7]
                    let is_inner4: u32 = (meta4 & (meta4 << 1)) & 0x10101010;
                    let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3);// (is_inner4 << 3) as i32 >> 31;
                    let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                    let child_bits4: u32 = (meta4 >> 5) & 0x07070707;

                    // n2.y == qlox[1]
                    // n3.w == qhix[1]

                    // n2.w == qloy[1]
                    // n4.y == qhiy[1]
    
                    // n3.y == qloz[1]
                    // n4.w == qhiz[1]

                    let swizzledLox = select(n2.y, n3.w, invdir.x < 0.0);
                    let swizzledHix = select(n3.w, n2.y, invdir.x < 0.0);

                    let swizzledLoy = select(n2.w, n4.y, invdir.y < 0.0);
                    let swizzledHiy = select(n4.y, n2.w, invdir.y < 0.0);

                    let swizzledLoz = select(n3.y, n4.w, invdir.z < 0.0);
                    let swizzledHiz = select(n4.w, n3.y, invdir.z < 0.0);

                    var tminx = vec4<f32>();
                    var tminy = vec4<f32>();
                    var tminz = vec4<f32>();
                    var tmaxx = vec4<f32>();
                    var tmaxy = vec4<f32>();
                    var tmaxz = vec4<f32>();

                    tminx[0] = f32((swizzledLox >>  0) & 0xFF) * adjusted_idirx + origx;
                    tminx[1] = f32((swizzledLox >>  8) & 0xFF) * adjusted_idirx + origx;
                    tminx[2] = f32((swizzledLox >> 16) & 0xFF) * adjusted_idirx + origx;
                    tminx[3] = f32((swizzledLox >> 24) & 0xFF) * adjusted_idirx + origx;

                    tminy[0] = f32((swizzledLoy >>  0) & 0xFF) * adjusted_idiry + origy;
                    tminy[1] = f32((swizzledLoy >>  8) & 0xFF) * adjusted_idiry + origy;
                    tminy[2] = f32((swizzledLoy >> 16) & 0xFF) * adjusted_idiry + origy;
                    tminy[3] = f32((swizzledLoy >> 24) & 0xFF) * adjusted_idiry + origy;

                    tminz[0] = f32((swizzledLoz >>  0) & 0xFF) * adjusted_idirz + origz;
                    tminz[1] = f32((swizzledLoz >>  8) & 0xFF) * adjusted_idirz + origz;
                    tminz[2] = f32((swizzledLoz >> 16) & 0xFF) * adjusted_idirz + origz;
                    tminz[3] = f32((swizzledLoz >> 24) & 0xFF) * adjusted_idirz + origz;

                    tmaxx[0] = f32((swizzledHix >>  0) & 0xFF) * adjusted_idirx + origx;
                    tmaxx[1] = f32((swizzledHix >>  8) & 0xFF) * adjusted_idirx + origx;
                    tmaxx[2] = f32((swizzledHix >> 16) & 0xFF) * adjusted_idirx + origx;
                    tmaxx[3] = f32((swizzledHix >> 24) & 0xFF) * adjusted_idirx + origx;

                    tmaxy[0] = f32((swizzledHiy >>  0) & 0xFF) * adjusted_idiry + origy;
                    tmaxy[1] = f32((swizzledHiy >>  8) & 0xFF) * adjusted_idiry + origy;
                    tmaxy[2] = f32((swizzledHiy >> 16) & 0xFF) * adjusted_idiry + origy;
                    tmaxy[3] = f32((swizzledHiy >> 24) & 0xFF) * adjusted_idiry + origy;

                    tmaxz[0] = f32((swizzledHiz >>  0) & 0xFF) * adjusted_idirz + origz;
                    tmaxz[1] = f32((swizzledHiz >>  8) & 0xFF) * adjusted_idirz + origz;
                    tmaxz[2] = f32((swizzledHiz >> 16) & 0xFF) * adjusted_idirz + origz;
                    tmaxz[3] = f32((swizzledHiz >> 24) & 0xFF) * adjusted_idirz + origz;

                    for (var i = 0u; i < 4u; i++)
                    {
                        // Use VMIN, VMAX to compute the slabs
                        let cmin = max(max(max(tminx[i], tminy[i]), tminz[i]), tmin);
                        let cmax = min(min(min(tmaxx[i], tmaxy[i]), tmaxz[i]), tmax);
                        let intersected = cmin <= cmax;

                        if intersected {
                            //intersections += 1;
                            hitmask |= extract_byte(child_bits4, i) << extract_byte(bit_index4, i);
                        }
                    }
                }
                //                                  take the 'inner node mask'
                //ngroup.y = (hitmask & 0xFF000000) | 255u;//extract_byte(n0.w, 0);
                ngroup.y = (hitmask & 0xFF000000) | (e_imask.w);//extract_byte(n0.w, 3);
                //ngroup.y = (hitmask & 0xFF000000) | (n0.w & 0xFF); 
                //extract_byte(n0.w, 0);
                tgroup.y = hitmask & 0x00FFFFFF;
            }
        }
        else {
            tgroup = ngroup;
            ngroup = vec2<u32>(0,0);
        }
        
        while tgroup.y != 0 {
            // Trace primitives
            // TODO: maybe add other types of primitives.
            // https://jcgt.org/published/0002/01/05/paper.pdf

            let triangleIndex = firstLeadingBit(tgroup.y);
            
            let triAddr = tgroup.x * 3u + triangleIndex * 3u;
            // triId += 1;

            let v00 = bitcast<vec4<f32>>(triangles[triAddr + 0u]);
            let v11 = bitcast<vec4<f32>>(triangles[triAddr + 1u]);
            let v22 = bitcast<vec4<f32>>(triangles[triAddr + 2u]);

            if intersect_bvh_leaf_triangle(orig, dir, v00.xyz, v11.xyz, v22.xyz, hit) {
                intersections +=1;

                if hit.uvwt.w < tmax {
                    tmax = hit.uvwt.w;
                    outcolor = randomColor(f32(triAddr));
                }
            }
            // let v00 = bitcast<vec4<f32>>(nodes[triAddr + 0]);
            // let v11 = bitcast<vec4<f32>>(nodes[triAddr + 1]);
            // let v22 = bitcast<vec4<f32>>(nodes[triAddr + 2]);

            // let oz = v00.w - orig.x*v00.x - orig.y*v00.y - orig.z*v00.z;
            // let invDz = 1.0 / (dir.x*v00.x + dir.y*v00.y + dir.z*v00.z);
            // let t = oz * invDz;

            // let ox = v11.w + orig.x*v11.x + orig.y*v11.y + orig.z*v11.z;
            // let dx = dir.x * v11.x + dir.y * v11.y + dir.z * v11.z;
            // let u = ox + t * dx;
            // let oy = v22.w + orig.x*v22.x + orig.y*v22.y + orig.z*v22.z;
            // let dy = dir.x*v22.x + dir.y*v22.y + dir.z*v22.z;
            // let v = oy + t*dy;

            // if (t > tmin && t < tmax)
            // {
            //     if (u >= 0.0 && u <= 1.0)
            //     {
            //         if (v >= 0.0 && u + v <= 1.0)
            //         {
            //             triangleuv.x = u;
            //             triangleuv.y = v;

            //             tmax = t;
            //             d = t;
            //             hitAddr = triAddr;
            //         }
            //     }
            // }

            tgroup.y &= ~(1u << triangleIndex);
        }

        if ngroup.y <= 0x00FFFFFF {
            if stackPtr > 0 {
                stackPtr -= 1;
                ngroup = traversalStack[stackPtr];
            }
            else {
                hit.uvwt = outcolor;//vec4<f32>(f32(intersections), f32(intersections), f32(intersections), 1.0);
                return;
            }
        }
    }
}

@compute @workgroup_size(16, 16, 1)
fn shot_ray_from_camera(@builtin(global_invocation_id) id: vec3<u32>) {
    let x = f32(id.x);
    let y = f32(id.y);
    
    let rayDir = vec4<f32>(generate_ray(vec2<f32>(x, y), vec2<f32>(f32(camera.width), f32(camera.height))).xyz, 999999999999999.0);
    let rayOrigin = vec4<f32>(camera.position.xyz, 0);

    var rayIntersection: RayIntersection;

    trace(rayOrigin, rayDir, &rayIntersection);

    // rayIntersection.uvwt.x /= 5.0;
    // rayIntersection.uvwt.y = 0;
    // rayIntersection.uvwt.z = 0;

    colorBuffer[id.y * camera.width + id.x] = rayIntersection.uvwt;
}

