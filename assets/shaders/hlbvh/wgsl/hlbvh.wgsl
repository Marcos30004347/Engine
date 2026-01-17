const U32_MAX = 4294967295u;

struct AABB {
    aabbMinX: f32,
    aabbMinY: f32,
    aabbMinZ: f32,
    aabbMaxX: f32,
    aabbMaxY: f32,
    aabbMaxZ: f32,
};

struct HLBVHNode {
    // TODO: make aabb min and max vec3<f32>
    // TODO: add triangle indices directly here
    // TODO: 
    parent: i32,

    left: i32,
    right: i32,
    //next: i32,

    aabbMinX: f32,
    aabbMinY: f32,
    aabbMinZ: f32,
    aabbMaxX: f32,
    aabbMaxY: f32,
    aabbMaxZ: f32,
};

struct Uniforms {
    num: i32
}

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var<storage, read_write> Mortoncodes: array<u32>;
@group(0) @binding(2) var<storage, read> Bounds: array<AABB>;
@group(0) @binding(3) var<storage, read> Indices: array<u32>;
@group(0) @binding(4) var<storage, read_write> Flags: array<atomic<u32>>;
@group(0) @binding(5) var<storage, read_write> Nodes: array<HLBVHNode>;




//@group(0) @binding(6) var<storage, read_write> lines: array<f32>;

// fn leaf_index(i: u32) -> u32 { 
//     return (uniforms.num - 1) + i; 
// }

fn leaf_index(i: i32) -> i32 { 
    return (uniforms.num - 1) + i; 
}

@compute @workgroup_size(64, 1, 1)
fn build_hierarchy(@builtin(global_invocation_id) gID: vec3<u32>) {
    let global_id = i32(gID.x);
       
    // Set leaf nodes
    if (global_id < uniforms.num) {
        let indice = Indices[global_id];

        Nodes[leaf_index(global_id)].left = i32(indice);
        Nodes[leaf_index(global_id)].right = i32(indice);

        Nodes[leaf_index(global_id)].aabbMaxX = Bounds[indice].aabbMaxX;
        Nodes[leaf_index(global_id)].aabbMaxY = Bounds[indice].aabbMaxY;
        Nodes[leaf_index(global_id)].aabbMaxZ = Bounds[indice].aabbMaxZ;
        Nodes[leaf_index(global_id)].aabbMinX = Bounds[indice].aabbMinX;
        Nodes[leaf_index(global_id)].aabbMinY = Bounds[indice].aabbMinY;
        Nodes[leaf_index(global_id)].aabbMinZ = Bounds[indice].aabbMinZ;
    }

    // Set internal nodes
    if (global_id < uniforms.num - 1) {
        let range = find_span(global_id);
        let split = find_split(range);

        var c1idx = select(split, leaf_index(split), split == range.x);
        var c2idx = select(split + 1,  leaf_index(split + 1), split + 1 == range.y);
        
        Nodes[(global_id)].left = c1idx;
        Nodes[(global_id)].right = c2idx;
        
        Nodes[c1idx].parent = global_id;
        Nodes[c2idx].parent = global_id;
    }
}


@compute @workgroup_size(64, 1, 1)
fn refit_bounds(@builtin(global_invocation_id) gID: vec3<u32>) {
    let global_id = i32(gID.x);

    if (global_id < uniforms.num) {
        var idx = leaf_index(global_id);

        loop {
            idx = Nodes[idx].parent;
            
            // TODO: flags is assumed to be initialized as 0 right now
            if (atomicCompareExchangeWeak(&Flags[idx], 0, 1).old_value == 1) {
                let lc = Nodes[idx].left;
                let rc = Nodes[idx].right;

                Nodes[idx].aabbMaxX = max(Nodes[lc].aabbMaxX, Nodes[rc].aabbMaxX);
                Nodes[idx].aabbMaxY = max(Nodes[lc].aabbMaxY, Nodes[rc].aabbMaxY);
                Nodes[idx].aabbMaxZ = max(Nodes[lc].aabbMaxZ, Nodes[rc].aabbMaxZ);
                Nodes[idx].aabbMinX = min(Nodes[lc].aabbMinX, Nodes[rc].aabbMinX);
                Nodes[idx].aabbMinY = min(Nodes[lc].aabbMinY, Nodes[rc].aabbMinY);
                Nodes[idx].aabbMinZ = min(Nodes[lc].aabbMinZ, Nodes[rc].aabbMinZ);
            } else {
                break;
            }

            if idx == 0 {
                break;
            }
        }
    }
}

fn delta(i1: i32, i2: i32) -> i32 {
    let left = min(i1, i2);
    let right = max(i1, i2);

    if (left < 0 || right >= uniforms.num) {
        return -1;
    }

    let leftcode = Mortoncodes[left];
    let rightcode = Mortoncodes[right];

    if (leftcode != rightcode) {
        return i32(countLeadingZeros(leftcode ^ rightcode));
    }

    return i32(32 + countLeadingZeros(left ^ right));
}

fn find_span(idx: i32) -> vec2<i32> {
    let d = sign(delta(idx, idx + 1) - delta(idx, idx - 1));
    let deltamin = delta(idx, idx - d);

    var lmax = 2;

    while (delta(idx, idx + lmax * d) > deltamin) {
        lmax *= 2;
    }

    var l = 0;
    var t = lmax;

    loop {
        t /= 2;
        if (delta(idx, idx + (l + t) * d) > deltamin) {
            l += t;
        }

        if t <= 1 {
            break;
        }
    }

    return vec2<i32>(min(idx, idx + l * d), max(idx, idx + l * d));
}

fn find_split(span: vec2<i32>) -> i32 {
    var left = span.x;
    var right = span.y;

    let numidentical = delta(left, right);

    loop {
        let newsplit = (right + left) / 2;
        if (delta(left, newsplit) > numidentical) {
            left = newsplit;
        } else {
            right = newsplit;
        }

        if !(right > left + 1) {
            break;
        }
    }

    return left;
}


// @compute @workgroup_size(64, 1, 1)
// fn encode_bvh_as_lines(@builtin(global_invocation_id) id: vec3<u32>) {

//     let global_id = i32(id.x);
//     // Set leaf nodes
//     if (global_id < uniforms.num) {
//         var index = leaf_index(global_id);

//         let node = Nodes[leaf_index(global_id)];

//         let minPoint = vec3<f32>(node.aabbMinX, node.aabbMinY, node.aabbMinZ);
//         let maxPoint = vec3<f32>(node.aabbMaxX, node.aabbMaxY, node.aabbMaxZ);

//         let vertices = array<vec3<f32>, 8>(
//             minPoint,
//             vec3<f32>(minPoint.x, minPoint.y, maxPoint.z),
//             vec3<f32>(minPoint.x, maxPoint.y, minPoint.z),
//             vec3<f32>(minPoint.x, maxPoint.y, maxPoint.z),
//             vec3<f32>(maxPoint.x, minPoint.y, minPoint.z),
//             vec3<f32>(maxPoint.x, minPoint.y, maxPoint.z),
//             vec3<f32>(maxPoint.x, maxPoint.y, minPoint.z),
//             maxPoint
//         );

//         // Define the 12 edges of the AABB using vertex indices
//         let edges = array<vec2<u32>, 12>(
//             vec2<u32>(0, 1), vec2<u32>(0, 2), vec2<u32>(0, 4), // edges from minPoint
//             vec2<u32>(1, 3), vec2<u32>(1, 5),                  // edges from vertex 1
//             vec2<u32>(2, 3), vec2<u32>(2, 6),                  // edges from vertex 2
//             vec2<u32>(3, 7),                                   // edges from vertex 3
//             vec2<u32>(4, 5), vec2<u32>(4, 6),                  // edges from vertex 4
//             vec2<u32>(5, 7),                                   // edges from vertex 5
//             vec2<u32>(6, 7)                                    // edges from vertex 6
//         );

//         // Each AABB contributes 12 lines (24 floats: start + end points of each line)
//         let baseIndex = index * 24 * 3; // 12 lines * 2 points * 3 components per point

//         for (var i = 0; i < 12; i = i + 1) {
//             let start = vertices[edges[i].x];
//             let end = vertices[edges[i].y];

//             let offset = baseIndex + i * 6; // Each line has 6 components: (start.x, start.y, start.z, end.x, end.y, end.z)

//             lines[offset + 0] = start.x;
//             lines[offset + 1] = start.y;
//             lines[offset + 2] = start.z;
//             lines[offset + 3] = end.x;
//             lines[offset + 4] = end.y;
//             lines[offset + 5] = end.z;
//         }
//     }

//     // Set internal nodes
//     if (global_id < i32(uniforms.num - 1)) {
//         var index = global_id;

//         let node = Nodes[global_id];

//         let minPoint = vec3<f32>(node.aabbMinX, node.aabbMinY, node.aabbMinZ);
//         let maxPoint = vec3<f32>(node.aabbMaxX, node.aabbMaxY, node.aabbMaxZ);

//         let vertices = array<vec3<f32>, 8>(
//             minPoint,
//             vec3<f32>(minPoint.x, minPoint.y, maxPoint.z),
//             vec3<f32>(minPoint.x, maxPoint.y, minPoint.z),
//             vec3<f32>(minPoint.x, maxPoint.y, maxPoint.z),
//             vec3<f32>(maxPoint.x, minPoint.y, minPoint.z),
//             vec3<f32>(maxPoint.x, minPoint.y, maxPoint.z),
//             vec3<f32>(maxPoint.x, maxPoint.y, minPoint.z),
//             maxPoint
//         );

//         // Define the 12 edges of the AABB using vertex indices
//         let edges = array<vec2<u32>, 12>(
//             vec2<u32>(0, 1), vec2<u32>(0, 2), vec2<u32>(0, 4), // edges from minPoint
//             vec2<u32>(1, 3), vec2<u32>(1, 5),                  // edges from vertex 1
//             vec2<u32>(2, 3), vec2<u32>(2, 6),                  // edges from vertex 2
//             vec2<u32>(3, 7),                                   // edges from vertex 3
//             vec2<u32>(4, 5), vec2<u32>(4, 6),                  // edges from vertex 4
//             vec2<u32>(5, 7),                                   // edges from vertex 5
//             vec2<u32>(6, 7)                                    // edges from vertex 6
//         );

//         // Each AABB contributes 12 lines (24 floats: start + end points of each line)
//         let baseIndex = index * 24 * 3; // 12 lines * 2 points * 3 components per point

//         for (var i = 0; i < 12; i = i + 1) {
//             let start = vertices[edges[i].x];
//             let end = vertices[edges[i].y];

//             let offset = baseIndex + i * 6; // Each line has 6 components: (start.x, start.y, start.z, end.x, end.y, end.z)

//             lines[offset + 0] = start.x;
//             lines[offset + 1] = start.y;
//             lines[offset + 2] = start.z;
//             lines[offset + 3] = end.x;
//             lines[offset + 4] = end.y;
//             lines[offset + 5] = end.z;
//         }
//     }

// }