const U32_MAX = 4294967295u;
const F32_MAX: f32 = 3.40282e+38;

struct Uniforms {
    num: u32,
    targetPrimitivesPerNode: u32,
    targetNumberOfLeaves: u32,
    padding: u32,
    volumeAABB: AABB,
}

struct AABB {
    minPoint: vec3<f32>,
    maxPoint: vec3<f32>,
};

//****************************************************
//                      HLBVH
//****************************************************

struct HLBVHNode {
    parent: u32,
    left: u32,
    right: u32,
    padding: u32,
    aabb: AABB,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var<storage, read> aabbs: array<AABB>;
@group(0) @binding(2) var<storage, read_write> inputLeafIndexRemap: array<u32>;
@group(0) @binding(3) var<storage, read> sorted_morton_codes: array<u32>;
@group(0) @binding(4) var<storage, read_write> flags: array<atomic<u32>>;
@group(0) @binding(5) var<storage, read_write> signals: array<atomic<u32>>;
@group(0) @binding(6) var<storage, read_write> hlbvhNodes: array<HLBVHNode>;
@group(0) @binding(7) var<storage, read_write> cwbvhNodes: array<CWBVHNode>;
@group(0) @binding(8) var<storage, read_write> outputLeafIndicesRemap: array<u32>;
@group(0) @binding(9) var<storage, read_write> debug : array<atomic<u32>>;



// WGSL version of the BVH construction shader

struct BVHNode {
    child0: u32,
    child1: u32,
    parent: u32,
    update: u32,
};

struct Transform {
    matrix: mat4x4<f32>,
};

struct InstanceDescription {
    data: vec4<u32>, // Adjust structure as needed
};

fn hlbvh_leaf_index(i: u32) -> u32 {
    return uniforms.targetNumberOfLeaves - 1u + i;
}

@compute @workgroup_size(64, 1, 1)
fn set_flags_to_zero(@builtin(global_invocation_id) gID: vec3<u32>) {
    atomicStore(&flags[2u * gID.x + 0u], 0u);
    atomicStore(&flags[2u * gID.x + 1u], 0u);
}

@compute @workgroup_size(64, 1, 1)
fn hlbvh_build_hierarchy(@builtin(global_invocation_id) gID: vec3<u32>) {
    let global_id = gID.x;

    if gID.x < uniforms.targetNumberOfLeaves {
        let indice = inputLeafIndexRemap[global_id];
        let lead_index = hlbvh_leaf_index(global_id);

        hlbvhNodes[lead_index].left = indice;
        hlbvhNodes[lead_index].right = indice;//sorted_morton_codes[global_id];

        var maxPoint = aabbs[indice].maxPoint;
        var minPoint = aabbs[indice].minPoint;

        let start = global_id * uniforms.targetPrimitivesPerNode;
        let end = min(start + uniforms.targetPrimitivesPerNode, uniforms.num);

        for(var i = start + 1u; i < end; i++) {
            maxPoint = max(maxPoint, aabbs[(inputLeafIndexRemap[i])].maxPoint);
            minPoint = min(minPoint, aabbs[(inputLeafIndexRemap[i])].minPoint);
        }

        hlbvhNodes[lead_index].aabb.maxPoint = maxPoint;
        hlbvhNodes[lead_index].aabb.minPoint = minPoint;
    }

    if gID.x < uniforms.targetNumberOfLeaves - 1u {
        let span = hlbvh_find_span(i32(global_id));
        let split = hlbvh_find_split(span);

        var addr_left: u32;
        var addr_right: u32;

        if split == span.x {
            addr_left = hlbvh_leaf_index(split);
        } else {
            addr_left = split;
        }

        if split + 1u == span.y {
            addr_right = hlbvh_leaf_index(split + 1u);
        } else {
            addr_right = split + 1u;
        }

        hlbvhNodes[global_id].left = addr_left;
        hlbvhNodes[global_id].right = addr_right;

        hlbvhNodes[addr_left].parent = global_id;
        hlbvhNodes[addr_right].parent = global_id;
    }
}

@compute @workgroup_size(64, 1, 1)
fn hlbvh_refit_aabbs(@builtin(global_invocation_id) gID: vec3<u32>) {
    let global_id = gID.x;

    if global_id < uniforms.targetNumberOfLeaves {
        var idx = hlbvh_leaf_index(global_id);

        loop {
            idx = hlbvhNodes[idx].parent;

            if atomicCompareExchangeWeak(&flags[idx], 0, 1).old_value == 1 {
                let lc = hlbvhNodes[idx].left;
                let rc = hlbvhNodes[idx].right;

                hlbvhNodes[idx].aabb.maxPoint = max(hlbvhNodes[lc].aabb.maxPoint, hlbvhNodes[rc].aabb.maxPoint);
                hlbvhNodes[idx].aabb.minPoint = min(hlbvhNodes[lc].aabb.minPoint, hlbvhNodes[rc].aabb.minPoint);
            } else {
                break;
            }

            if idx == 0u {
                break;
            }
        }
    }
}

// fn hlbvh_delta(i1: i32, i2: i32) -> i32 {
//     let left = min(i1, i2);
//     let right = max(i1, i2);

//     if (left < 0 || right >= i32(uniforms.targetNumberOfLeaves)) {
//         return -1;
//     }

//     let leftcode = sorted_morton_codes[left];
//     let rightcode = sorted_morton_codes[right];

//     if (leftcode != rightcode) {
//         return i32(countLeadingZeros(leftcode ^ rightcode));
//     }

//     return 32 + countLeadingZeros(left ^ right);
// }

// fn hlbvh_find_span(idx: i32) -> vec2<i32> {
//     let d = sign(hlbvh_delta(idx, idx + 1) - hlbvh_delta(idx, idx - 1));
//     let deltamin = hlbvh_delta(idx, idx - d);

//     let lmax = i32((1u << countLeadingZeros(u32(abs(idx)))) >> 1u);

//     var l = 0;
//     var step = lmax;

//     while step > 0 {
//         if (hlbvh_delta(idx, idx + (l + step) * d) > deltamin) {
//             l += step;
//         }
//         step >>= 1u;
//     }

//     return vec2<i32>(min(idx, idx + l * d), max(idx, idx + l * d));
// }

fn hlbvh_find_split(span: vec2<u32>) -> u32 {
    var left = i32(span.x);
    var right = i32(span.y);

    let num_identical = hlbvh_commom_prefix_length(left, right);

    while right > left + 1 {
        let new_split = (right + left) / 2;
        if hlbvh_commom_prefix_length(left, new_split) > num_identical {
            left = new_split;
        } else {
            right = new_split;
        }
    }

    return u32(left);
}

// fn hlbvh_find_split(span: vec2<i32>) -> i32 {
//     var left = span.x;
//     var right = span.y;

//     let numidentical = hlbvh_delta(left, right);

//     loop {
//         let newsplit = (right + left) / 2;
//         if (hlbvh_delta(left, newsplit) > numidentical) {
//             left = newsplit;
//         } else {
//             right = newsplit;
//         }

//         if !(right > left + 1) {
//             break;
//         }
//     }

//     return left;
// }
fn clz(v: u32) -> u32 {
    return 32u - countLeadingZeros(v);
}
fn hlbvh_commom_prefix_length(i1: i32, i2: i32) -> i32 {
    let left = min(i1, i2);
    let right = max(i1, i2);

    if left < 0 || right >= i32(uniforms.targetNumberOfLeaves) {
        return 0;
    }

    let left_code = sorted_morton_codes[left];
    let right_code = sorted_morton_codes[right];

    if left_code != right_code {
        return i32(countLeadingZeros(left_code ^ right_code));
    } 

    return 32 + i32(countLeadingZeros(u32(left ^ right)));
}

fn hlbvh_find_span(index: i32) -> vec2<u32> {
    let d = sign(hlbvh_commom_prefix_length(index, index + 1) - hlbvh_commom_prefix_length(index, index - 1));
    let delta_min = hlbvh_commom_prefix_length(index, index - d);
    
    var lmax = 2;
    
    while hlbvh_commom_prefix_length(index, index + lmax * d) > delta_min {
        lmax *= 2;
    }
    
    var l = 0;
    var t = lmax;
    
    while t > 1 {
        t /= 2;
        if hlbvh_commom_prefix_length(index, index + (l + t) * d) > delta_min {
            l += t;
        }
    }
    
    let v0 = u32(min(index, index + l * d));
    let v1 = u32(max(index, index + l * d));

    return vec2<u32>(v0, min(v1, uniforms.targetNumberOfLeaves - 1u));
}


// fn hlbvh_find_split(span: vec2<i32>) -> i32 {
//     var left = span.x;
//     var right = span.y;

//     let numidentical = hlbvh_delta(left, right);

//     loop {
//         let newsplit = (right + left) / 2;
//         if (hlbvh_delta(left, newsplit) > numidentical) {
//             left = newsplit;
//         } else {
//             right = newsplit;
//         }

//         if !(right > left + 1) {
//             break;
//         }
//     }

//     return left;
// }

//****************************************************
//                      CWBVH
//****************************************************

struct CWBVHNode {  
    // To interpret "info" field:
    //   Empty child slot: field set to 00000000
    //   Internal node: high 3 bits 001 while low 5 bits store child slot index + 24. (values range in 24-31)
    //   Leaf node: high 3 bits store number of triangles using unary encoding, and low 5 bits store the
    //              index of first triangle relative to the triangle base index (ranging in 0...23)

    // [32b origin x] [32b origin y]
    orxy: array<u32, 2>, // n0

    // [32b origin z] [8b extent x] [8b extent y] [8b extent z] [8b inner node mask]
    oeim: array<u32, 2>,

    // [32b child node base index] [32b triangle base index]
    ntbi: array<u32, 2>, // n1

    // [8b child 0] [8b child 1] [8b child 2] [8b child 3] [8b child 4] [8b child 5] [8b child 6] [8b child 7]
    info: array<u32, 2>,
    qlox: array<u32, 2>, // n2.x
    qloy: array<u32, 2>, // n2.z
    qloz: array<u32, 2>, // n3.x
    qhix: array<u32, 2>, // n3.z
    qhiy: array<u32, 2>, // n4.x
    qhiz: array<u32, 2>, // n4.z
};



struct Pair {
    key: u32,   // The key of the pair
    value: f32, // The value used to balance the heap
};

struct MaxHeap {
    data: array<Pair, 8>, // Fixed size heap of 8 pairs
    count: i32,           // Current size of the heap
};

// fn load_bounds(nodeId: u32) -> mat2x3<f32> {
//     return mat2x3<f32>(hlbvhNodes[nodeId].aabb.min, hlbvhNodes[nodeId].aabb.max,);
// }

fn get_surface_area(aabb: AABB) -> f32 {
    let d = aabb.maxPoint - aabb.minPoint;
    return max(2.0f * (d.x * d.y + d.x * d.z + d.y * d.z), 0.0f);
}

fn heap_size(heap: ptr<private, MaxHeap>) -> i32 {
    return (*heap).count;
}

fn heap_push(heap: ptr<private, MaxHeap>, key: u32, value: f32) {
    var current = (*heap).count;

    // Check if the heap is full
    if current >= 8 {
        return;
    }

    // Insert the new pair at the end of the heap
    (*heap).data[current].key = key;
    (*heap).data[current].value = value;
    (*heap).count += 1;

    // Bubble up
    while current > 0 {
        let parent = (current - 1) / 2;
        if (*heap).data[parent].value >= (*heap).data[current].value {
            break;
        }

        // Swap parent and current
        let temp = (*heap).data[parent];
        (*heap).data[parent] = (*heap).data[current];
        (*heap).data[current] = temp;

        current = parent;
    }
}

fn heap_pop(heap: ptr<private, MaxHeap>) -> u32 {
    if (*heap).count <= 0 {
        return U32_MAX; // Return -1 to indicate an empty heap
    }

    let result = (*heap).data[0].key; // Store the key of the root element to return later
    (*heap).count -= 1;

    // Move the last element to the root
    (*heap).data[0] = (*heap).data[(*heap).count];

    // Bubble down
    var current = 0;
    while true {
        let left = current * 2 + 1;
        let right = current * 2 + 2;
        var largest = current;

        if left < (*heap).count && (*heap).data[left].value > (*heap).data[largest].value {
            largest = left;
        }
        if right < (*heap).count && (*heap).data[right].value > (*heap).data[largest].value {
            largest = right;
        }

        if largest == current {
            break;
        }

        // Swap current and largest
        let temp = (*heap).data[current];
        (*heap).data[current] = (*heap).data[largest];
        (*heap).data[largest] = temp;

        current = largest;
    }

    return result;
}

fn heap_clear(heap: ptr<private, MaxHeap>) {
    (*heap).count = 0;
}



var<private> heap : MaxHeap;
var<private> indexTable = array<u32, 8>(0, 0, 0, 0, 1, 1, 1, 1);
var<private> offsetTable = array<u32, 8>(0, 8, 16, 24, 0, 8, 16, 24);

@compute @workgroup_size(64, 1, 1)
fn cwbvh_initialize(@builtin(global_invocation_id) id: vec3<u32>) {
    if id.x == 0u {
        atomicStore(&flags[id.x * 2u + 0u], 0u);//(GEOM_ID_BVH2 << 24u);
        atomicStore(&flags[id.x * 2u + 1u], 0u);
        atomicStore(&signals[0], 0u);
        atomicStore(&signals[1], 0u);
        atomicStore(&signals[2], 0u);
        atomicStore(&signals[3], 1u);
        atomicStore(&signals[4], 0u);
        atomicStore(&signals[5], 0u);
    } else {
        atomicStore(&flags[id.x * 2u + 0u], U32_MAX);
        atomicStore(&flags[id.x * 2u + 1u], U32_MAX);
    }
}


@compute @workgroup_size(64, 1, 1)
fn cwbvh_build(@builtin(global_invocation_id) id: vec3<u32>) {
    let threadId = id.x;

    if threadId >= uniforms.targetNumberOfLeaves {
        atomicStore(&debug[threadId * 8u + 7u], 4u);
        return;
    }

    loop {
        if atomicLoad(&signals[4]) == uniforms.num {
            atomicStore(&debug[threadId * 8u + 7u], 3u);
            return;
        }

        let bvh2Node = atomicLoad(&flags[threadId * 2u + 0u]);
        let bvh8Node = atomicLoad(&flags[threadId * 2u + 1u]);

        if bvh2Node == U32_MAX || bvh8Node == U32_MAX {
            continue;
        }

        var childs = array<u32, 8>(U32_MAX, U32_MAX, U32_MAX, U32_MAX, U32_MAX, U32_MAX, U32_MAX, U32_MAX);
        var head = 0u;

        heap_clear(&heap);

        heap_push(&heap, hlbvhNodes[bvh2Node].left, get_surface_area(hlbvhNodes[bvh2Node].aabb));
        heap_push(&heap, hlbvhNodes[bvh2Node].right, get_surface_area(hlbvhNodes[bvh2Node].aabb));

        while heap_size(&heap) > 0 && (heap_size(&heap) + i32(head)) < 8 {
            let nodeId = heap_pop(&heap);

            if hlbvhNodes[nodeId].left == hlbvhNodes[nodeId].right {
                childs[head] = nodeId;
                head += 1u;
            } else {
                heap_push(&heap, hlbvhNodes[nodeId].left, get_surface_area(hlbvhNodes[nodeId].aabb));
                heap_push(&heap, hlbvhNodes[nodeId].right, get_surface_area(hlbvhNodes[nodeId].aabb));
            }
        }

        while heap_size(&heap) > 0 {
            childs[head] = heap_pop(&heap);
            head += 1u;
        }

        var isLeaf = array<bool, 8>(false, false, false, false, false, false, false, false);
        var internalCount = 0u;
        var leavesCount = 0u;

        for (var i = 0u; i < head; i += 1u) {
            if hlbvhNodes[(childs[i])].left == hlbvhNodes[(childs[i])].right {
                isLeaf[i] = true;
                leavesCount += 1u;
            } else {
                isLeaf[i] = false;
                internalCount += 1u;
            }
        }

        var parentBounds = hlbvhNodes[bvh2Node].aabb;

            { // Sort phase
            var parentCenter = (parentBounds.maxPoint + parentBounds.minPoint) * 0.5;

            var childCenters = array<vec3<f32>, 8>();

            for (var i = 0u; i < head; i++) {
                childCenters[i] = (hlbvhNodes[(childs[i])].aabb.maxPoint + hlbvhNodes[(childs[i])].aabb.minPoint) * 0.5;
            }

            var cost = array<array<f32, 8>, 8>();

            var assignments = array<u32, 8>(U32_MAX, U32_MAX, U32_MAX, U32_MAX, U32_MAX, U32_MAX, U32_MAX, U32_MAX);
            var isSlotEmpty = array<bool, 8>(true, true, true, true, true, true, true, true);

            for (var childIndex = 0u; childIndex < 8u; childIndex++) {
                for (var s = 0; s < 8; s++) {
                    var direction = vec3<f32>();

                    direction.x = select(1.0, -1.0, (s & 4) == 1);
                    direction.y = select(1.0, -1.0, (s & 2) == 1);
                    direction.z = select(1.0, -1.0, (s & 1) == 1);

                    cost[s][childIndex] = dot(childCenters[childIndex] - parentCenter, direction);
                }
            }

            // Greedy ordering, TODO: add sort
            while true {
                var minCost = F32_MAX;
                var minSlot = U32_MAX;
                var minIndex = U32_MAX;

                for (var c = 0u; c < head; c++) {
                    if assignments[c] == U32_MAX {
                        for (var s = 0u; s < 8u; s++) {
                            if isSlotEmpty[s] && cost[s][c] < minCost {
                                minCost = cost[s][c];
                                minSlot = s;
                                minIndex = c;
                            }
                        }
                    }
                }

                if minSlot == U32_MAX {
                    break;
                }

                isSlotEmpty[minSlot] = false;
                assignments[minIndex] = minSlot;
            }

            var orderedChilds = array<u32, 8>();
            var orderedLeafs = array<bool, 8>();
            var orderedBounds = array<mat2x3<f32>, 8>();

            for (var i = 0u; i < 8u; i++) {
                orderedChilds[i] = childs[i];
                orderedLeafs[i] = isLeaf[i];

                childs[i] = U32_MAX;
                isLeaf[i] = false;
            }

            for (var i = 0u; i < head; i++) {
                childs[(assignments[i])] = orderedChilds[i];
                isLeaf[(assignments[i])] = orderedLeafs[i];
            }
        }

        var e = ceil(log2((parentBounds.maxPoint - parentBounds.minPoint) / 255.0));

        var node = CWBVHNode();

        let childNodeBaseIndex = atomicAdd(&signals[3], internalCount);
        let primitiveBaseIndex = atomicAdd(&signals[4], uniforms.targetPrimitivesPerNode * leavesCount);

        var imask = 0u;

        if internalCount + leavesCount > 0u {
            let increment = internalCount + leavesCount - 1u;
            let baseWorkerOffset = atomicAdd(&signals[2], increment);

            var nodeOffset = 0u;
            var primitiveOffset = 0u;

            var bvh8Index = U32_MAX;
            var bvh2Index = U32_MAX;

            var twoToE = vec3<f32>(pow(2.0, e.x), pow(2.0, e.y), pow(2.0, e.z));

            var internalLeafOffset = 0u;

            for (var i = 0u; i < 8u; i++) {
                if childs[i] == U32_MAX {
                    continue;
                }

                var workerAddr = threadId;

                if nodeOffset + primitiveOffset > 0u {
                    workerAddr = baseWorkerOffset + primitiveOffset + nodeOffset ;
                }

                var p0 = hlbvhNodes[(childs[i])].aabb.minPoint - parentBounds.minPoint;
                var p1 = hlbvhNodes[(childs[i])].aabb.maxPoint - parentBounds.minPoint;

                let qlo = floor(p0 / twoToE);
                let qhi = ceil(p1 / twoToE);

                let index = indexTable[i];
                let offset = offsetTable[i];

                node.qlox[index] |= u32(qlo.x) << offset;
                node.qloy[index] |= u32(qlo.y) << offset;
                node.qloz[index] |= u32(qlo.z) << offset;
                node.qhix[index] |= u32(qhi.x) << offset;
                node.qhiy[index] |= u32(qhi.y) << offset;
                node.qhiz[index] |= u32(qhi.z) << offset;

                if !isLeaf[i] {
                    imask |= (1u << i);
                    
                    // Note that the index in the trace function if computed by bit count,  not by the index + 24 stored here.
                    // This is rather the slot index.
                    let metaField = (1u << 5u) | ((i + 24u) & 0x1fu);
                    node.info[index] |= metaField << offset;

                    atomicStore(&flags[workerAddr * 2u + 0u], childs[i]);
                    atomicStore(&flags[workerAddr * 2u + 1u], childNodeBaseIndex + nodeOffset);

                    nodeOffset += 1u;
                } else {
                    var primitiveCount = 1u;

                    // let limit = min(uniforms.num, hlbvhNodes[(childs[i])].left + uniforms.targetPrimitivesPerNode);
                    //let primitiveCurrIndex = hlbvhNodes[(childs[i])].left;
                    
                    outputLeafIndicesRemap[primitiveBaseIndex + primitiveOffset + primitiveCount] = hlbvhNodes[(childs[i])].left;

                    // for (var aabbIndex = hlbvhNodes[(childs[i])].left; aabbIndex < limit; aabbIndex++) {
                    //     outputLeafIndicesRemap[primitiveBaseIndex + primitiveOffset + primitiveCount] = inputLeafIndexRemap[aabbIndex];
                    //     primitiveOffset += 1u;
                    //     primitiveCount += 1u;
                    // }

                    var unaryEncode = 1u;

                    switch primitiveCount {
                        case 2u : {
                            unaryEncode = 3u;
                        }
                        case 3u : {
                            unaryEncode = 7u;
                        }
                        default: {
                            unaryEncode = 1u;
                        }
                    }

                    let metaField = (unaryEncode << 5u) | internalLeafOffset;

                    internalLeafOffset += primitiveCount;

                    node.info[index] |= metaField << offset;
                }
            }
        }

        let sxExp = bitcast<u32>(i32(e.x)) & 0xFFu;
        let syExp = bitcast<u32>(i32(e.y)) & 0xFFu;
        let szExp = bitcast<u32>(i32(e.z)) & 0xFFu;

        node.orxy[0] = bitcast<u32>(parentBounds.minPoint.x);
        node.orxy[1] = bitcast<u32>(parentBounds.minPoint.y);
        node.oeim[0] = bitcast<u32>(parentBounds.minPoint.z);

        var oeim = vec4<u32>(sxExp, syExp, szExp, imask);

        node.oeim[1] = pack4xU8(oeim); // (sxExp << 24u) | (syExp << 16u) | (szExp << 8u) | (imask);

        node.ntbi[0] = childNodeBaseIndex;

        node.ntbi[1] = primitiveBaseIndex;
        cwbvhNodes[bvh8Node] = node;

        if internalCount == 0u {
            atomicStore(&debug[threadId * 8u + 7u], 2u);
            return;
        }
    }

    atomicStore(&debug[threadId * 8u + 7u], 1u);
}


