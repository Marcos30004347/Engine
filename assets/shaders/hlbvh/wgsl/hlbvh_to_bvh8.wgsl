enable subgroups;

const U32_MAX = 4294967295u;
const F32_MAX: f32 = 3.40282e+38;


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

    left: u32,
    right: u32,
    // next: i32,

    aabbMinX: f32,
    aabbMinY: f32,
    aabbMinZ: f32,
    aabbMaxX: f32,
    aabbMaxY: f32,
    aabbMaxZ: f32,
};

struct BVH8Node {  
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

struct BVH8Leaf {
    v0: vec4<f32>,
    v1: vec4<f32>,
    v2: vec4<f32>,
};

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


struct Uniforms {
    num: u32,
    numBVH2Nodes: u32,
}

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var<storage, read_write> signals: array<atomic<u32>>;
@group(0) @binding(2) var<storage, read_write> indexPairs: array<u32>;
@group(0) @binding(3) var<storage, read> nodes: array<HLBVHNode>;
@group(0) @binding(4) var<storage, read_write> bvh8Nodes: array<BVH8Node>;
@group(0) @binding(5) var<storage, read> triangles : array<Triangle>;
@group(0) @binding(6) var<storage, read> vertices : array<vec4<f32>>;
@group(0) @binding(7) var<storage, read_write> ouTtriangles : array<vec4<f32>>;
@group(0) @binding(8) var<storage, read_write> debug : array<atomic<u32>>;

//@group(1) @binding(1) var<storage, read_write> debug : array<i32>;

// void WoopifyTriangle(
// 	float3 v0,
// 	float3 v1,
// 	float3 v2,
// 	float4& OutWoopifiedV0,
// 	float4& OutWoopifiedV1,
// 	float4& OutWoopifiedV2
// )
// {
// 	Mat4f mtx;
// 	float4 col0 = make_float4(v0 - v2, 0.0f);
// 	float4 col1 = make_float4(v1 - v2, 0.0f);
// 	float4 col2 = make_float4(cross(v0 - v2, v1 - v2), 0.0f);
// 	float4 col3 = make_float4(v2, 1.0f);
// 	mtx.setCol(0, Vec4f(col0));
// 	mtx.setCol(1, Vec4f(col1));
// 	mtx.setCol(2, Vec4f(col2));
// 	mtx.setCol(3, Vec4f(col3));
// 	mtx = invert(mtx);

// 	OutWoopifiedV0 = make_float4(mtx(2, 0), mtx(2, 1), mtx(2, 2), -mtx(2, 3));
// 	OutWoopifiedV1 = make_float4(mtx.getRow(0).x, mtx.getRow(0).y, mtx.getRow(0).z, mtx.getRow(0).w);
// 	OutWoopifiedV2 = make_float4(mtx.getRow(1).x, mtx.getRow(1).y, mtx.getRow(1).z, mtx.getRow(1).w);
// }



struct Pair {
    key: u32,   // The key of the pair
    value: f32, // The value used to balance the heap
};

struct MaxHeap {
    data: array<Pair, 8>, // Fixed size heap of 8 pairs
    count: i32,           // Current size of the heap
};

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

// struct ChildStack {
//     leaves : array<u32, 8u>,
//     nodes : array<u32, 8u>,
//     numLeaves: u32,
//     numNodes: u32,
// };

// fn child_stack_size(child_stack: ptr<function, ChildStack>) -> u32 {
//     return child_stack.numNodes + child_stack.numLeaves;
// }
// fn child_stack_push_leaf(child_stack: ptr<function, ChildStack>, leaf: u32) -> bool {
//     if child_stack.numNodes + child_stack.numLeaves >= 8u {
//         return false;
//     }
//     child_stack.leaves[child_stack.numLeaves] = leaf;
//     child_stack.numLeaves += 1u;
//     return true;
// }
// fn child_stack_push_node(child_stack: ptr<function, ChildStack>, node: u32) -> bool {
//     if child_stack.numNodes + child_stack.numLeaves >= 8u {
//         return false;
//     }
//     child_stack.nodes[child_stack.numNodes] = node;
//     child_stack.numNodes += 1u;

//     return true;
// }
// fn child_stack_push_cluster(child_stack: ptr<function, ChildStack>, clusterId: u32) -> bool {
//     if nodes[clusterId].left == nodes[clusterId].right {
//     //if (clusterId >> 24u) == GEOM_ID_BVH2 {
//         return child_stack_push_leaf(child_stack, clusterId);
//     }
//     return child_stack_push_node(child_stack, clusterId);
// }
// fn child_stack_pop(child_stack: ptr<function, ChildStack>) -> u32 {
//     if child_stack.numNodes == 0u {
//         return U32_MAX;
//     }

//     child_stack.numNodes -= 1u;
//     return child_stack.nodes[child_stack.numNodes];
// }
// fn child_stack_can_open_inner_node(child_stack: ptr<function, ChildStack>) -> bool {
//     let hasRoom = child_stack.numNodes + child_stack.numLeaves < 8u;
//     let hasIneerNode = child_stack.numNodes > 0u;
//     return hasRoom && hasIneerNode;
// }



fn load_bounds(nodeId: u32) -> mat2x3<f32> {
    return mat2x3<f32>(
        vec3<f32>(nodes[nodeId].aabbMinX, nodes[nodeId].aabbMinY, nodes[nodeId].aabbMinZ),
        vec3<f32>(nodes[nodeId].aabbMaxX, nodes[nodeId].aabbMaxY, nodes[nodeId].aabbMaxZ),
    );
}

fn get_surface_area(bounds: mat2x3<f32>) -> f32 {
    let d = bounds[1] - bounds[0];
    return max(2.0f * (d.x * d.y + d.x * d.z + d.y * d.z), 0.0f);
}

// fn float_to_exponent(num: f32) -> u32 {
//     let bits = bitcast<u32>(num);
//     let exponentBits = (bits >> 23u) & 0xFF; // Mask to get only the exponent bits
//     // Avoiding 255, since that's infinity.
//     return u32(max(min(exponentBits + 1u, 254u), 2u));
// }

// fn exponent_to_float(exponent: u32) -> f32 {
//     let bits = exponent << 23;
//     return bitcast<f32>(bits);
// }

// fn get_imask(assignedChildren: array<i32, 8>, numLeaves: u32) -> u32 {
//     var imask = 0u;
//     for (var i = 0u; i < 8u; i++) {
//         if assignedChildren[i] != -1 {
//             let relativeIndex = assignedChildren[i];
//             if u32(relativeIndex) >= numLeaves {
//                 imask |= (1u << i);
//             }
//         }
//     }
//     return imask;
// }

// fn bitwise_shift_left_u64(high: u32, low: u32, shift: u32, high_res: ptr<function, u32>, low_res: ptr<function, u32>) {
//     if shift >= 32 {
//         *high_res = low << (shift - 32);
//         *low_res = 0;
//     } else {
//         *high_res = (high << shift) | (low >> (32 - shift));
//         *low_res = low << shift;
//     }
// }

// fn bitwise_or_u64(high1: u32, low1: u32, high2: u32, low2: u32, high_res: ptr<function, u32>, low_res: ptr<function, u32>) {
//     *low_res = low1 | low2;
//     *high_res = high1 | high2;
// }

@compute @workgroup_size(64, 1, 1)
fn initializeIndexPairs(@builtin(global_invocation_id) id: vec3<u32>) {
    if id.x == 0u {
        indexPairs[id.x * 2u + 0u] = 0u;//(GEOM_ID_BVH2 << 24u);
        indexPairs[id.x * 2u + 1u] = 0u;
        atomicStore(&signals[0], 0u);
        atomicStore(&signals[1], 0u);
        atomicStore(&signals[2], 0u);
        atomicStore(&signals[3], 1u);
        atomicStore(&signals[4], 0u);
        atomicStore(&signals[5], 0u);
    } else {
        indexPairs[id.x * 2u + 0u] = U32_MAX;
        indexPairs[id.x * 2u + 1u] = U32_MAX;
    }
}

var<private> heap : MaxHeap;
var<private> indexTable = array<u32, 8>(0, 0, 0, 0, 1, 1, 1, 1);
var<private> offsetTable = array<u32, 8>(0, 8, 16, 24, 0, 8, 16, 24);


@compute @workgroup_size(64, 1, 1)
fn collect_childs(@builtin(global_invocation_id) id: vec3<u32>) {
    let threadId = id.x;

    if threadId >= uniforms.num {
        debug[threadId * 8u + 7u] = 2u;
        return;
    }

    loop {
        if indexPairs[threadId * 2u + 0u] == U32_MAX || indexPairs[threadId * 2u + 1u] == U32_MAX {
            // if atomicLoad(&signals[4]) == uniforms.num {
            //     atomicStore(&debug[16u * threadId + 0u], threadId);
            //     atomicStore(&debug[16u * threadId + 1u], 3u);
            //     return;
            // }

            continue;
        }

        
        let bvh2Node = indexPairs[threadId * 2u + 0u];//indexPairs[threadId * 2u + 0u];
        let bvh8Node = indexPairs[threadId * 2u + 1u];//indexPairs[threadId * 2u + 1u];
        
        indexPairs[threadId * 2u + 0u] = U32_MAX;
        indexPairs[threadId * 2u + 1u] = U32_MAX;
        
        if nodes[bvh2Node].left == nodes[bvh2Node].right {
            // atomicStore(&debug[16u * threadId + 0u], threadId);
            // atomicStore(&debug[16u * threadId + 1u], 3u);
            return;     
        }
       
        var childs = array<u32, 8>(U32_MAX, U32_MAX, U32_MAX, U32_MAX, U32_MAX, U32_MAX, U32_MAX, U32_MAX);
        var head = 0u;

        heap_clear(&heap);

        heap_push(&heap, nodes[bvh2Node].left, get_surface_area(load_bounds(nodes[bvh2Node].left)));
        heap_push(&heap, nodes[bvh2Node].right, get_surface_area(load_bounds(nodes[bvh2Node].right)));

        while heap_size(&heap) > 0 && (heap_size(&heap) + i32(head)) < 8 {
            let nodeId = heap_pop(&heap);

            if nodes[nodeId].left == nodes[nodeId].right {
                childs[head] = nodeId;
                head += 1u;
            } else {
                heap_push(&heap, nodes[nodeId].left, get_surface_area(load_bounds(nodes[nodeId].left)));
                heap_push(&heap, nodes[nodeId].right, get_surface_area(load_bounds(nodes[nodeId].right)));
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
            if nodes[(childs[i])].left == nodes[(childs[i])].right {
                isLeaf[i] = true;
                leavesCount += 1u;
            } else {
                isLeaf[i] = false;
                internalCount += 1u;
            }
        }



        // atomicAdd(&signals[5], leavesCount);
        var parentBounds = load_bounds(bvh2Node);
        var childBounds = array<mat2x3<f32>, 8>();

        {
            var parentCenter = (parentBounds[0] + parentBounds[1]) * 0.5;

            var childCenters = array<vec3<f32>, 8>();

            for (var i = 0u; i < head; i++) {
                var bounds = load_bounds(childs[i]);
                childBounds[i][0] = bounds[0];
                childBounds[i][1] = bounds[1];
                childCenters[i] = (bounds[0] + bounds[1]) * 0.5;
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
                    if(assignments[c] == U32_MAX) {
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

            var orderedChilds =  array<u32, 8>();
            var orderedLeafs =  array<bool, 8>();
            var orderedBounds = array<mat2x3<f32>, 8>();

            for(var i = 0u; i < 8u; i++) {
                orderedChilds[i] = childs[i];
                orderedLeafs[i] = isLeaf[i];
                orderedBounds[i] = childBounds[i];

                childs[i] = U32_MAX;
                isLeaf[i] = false;
            }

            for (var i = 0u; i < head; i++) {
                childs[(assignments[i])] = orderedChilds[i];
                isLeaf[(assignments[i])] = orderedLeafs[i];
                childBounds[(assignments[i])] = orderedBounds[i];
            }
        }

        var e = ceil(log2((parentBounds[1] - parentBounds[0]) / 255.0));

        var node = BVH8Node();

        let childNodeBaseIndex = atomicAdd(&signals[3], internalCount);
        let primitiveBaseIndex = atomicAdd(&signals[4], leavesCount);
        var imask = 0u;

        if internalCount + leavesCount > 0u {
            let increment = internalCount + leavesCount - 1u;
            let baseWorkerOffset = atomicAdd(&signals[2], increment);

            var nodeOffset = 0u;
            var primitiveOffset = 0u;
            
            var bvh8Index = U32_MAX;
            var bvh2Index = U32_MAX;
            
            var twoToE = vec3<f32>(pow(2.0, e.x), pow(2.0, e.y),pow(2.0, e.z));
    
            var internalLeafOffset = 0u;
            
            for (var i = 0u; i < 8u; i++) {
                if childs[i] == U32_MAX {
                    continue;
                }

                var workerAddr = threadId;

                if nodeOffset + primitiveOffset > 0u {
                    workerAddr = baseWorkerOffset + primitiveOffset + nodeOffset ;
                }

                var p0 = childBounds[i][0] - parentBounds[0];
                var p1 = childBounds[i][1] - parentBounds[0];

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
 
                // atomicStore(&debug[16u * threadId + 3u], threadId);
                //atomicStore(&debug[16u * threadId + 3u + i], workerAddr);
                atomicStore(&debug[16u * workerAddr + 15u], 1u);

                if !isLeaf[i] {
                    imask |= (1u << i);
                    
                    // Note that the index in the trace function if computed by bit count,  not by the index + 24 stored here.
                    // This is rather the slot index.
                    let metaField = (1u << 5u) | ((i + 24u) & 0x1fu);
                    node.info[index] |= metaField << offset;

                    indexPairs[workerAddr * 2u + 0u] = childs[i];
                    indexPairs[workerAddr * 2u + 1u] = childNodeBaseIndex + nodeOffset;
                    
                    nodeOffset += 1u;
                }
                else {
                    var j = 1u;

                    indexPairs[workerAddr * 2u + 0u] = childs[i];
                    indexPairs[workerAddr * 2u + 1u] = primitiveBaseIndex + primitiveOffset;
                    
                    primitiveOffset += 1u;

                    // TODO: we can reference up to three triangles using a single child, 
                    // j is the number of triangles and will be unary encoded. The problem
                    // currently is that because of our HLBVH layout each node store a single
                    // triangle and because of our expansion, we can only have up to 8 triangles
                    // per CWBVH node. If we extend our HLBVH to store more than 1 triangle per 
                    // node we can solve this problem.

                    // while(isLeaf[i + j] && ((i + j) < 8u) && j < 3u && (childs[i] == childs[i + j] + j)) {
                    //     indexPairs[workerAddr * 2u + 0u] = childs[i];
                    //     indexPairs[workerAddr * 2u + 1u] = primitiveBaseIndex + primitiveOffset;
                    
                    //     workerAddr = baseWorkerOffset + nodeOffset + primitiveOffset;
                    //     primitiveOffset += 1u;
                    //     j += 1u;
                    // }
                    
                    var unaryEncode = 1u;
                    
                    switch j {
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

                    internalLeafOffset += j;

                    node.info[index] |= metaField << offset;

                    //i += j - 1u;
                }
            }

        }

        let sxExp = bitcast<u32>(i32(e.x)) & 0xFFu;
        let syExp = bitcast<u32>(i32(e.y)) & 0xFFu;
        let szExp = bitcast<u32>(i32(e.z)) & 0xFFu;
        
        node.orxy[0] = bitcast<u32>(parentBounds[0].x);
        node.orxy[1] = bitcast<u32>(parentBounds[0].y);
        node.oeim[0] = bitcast<u32>(parentBounds[0].z);
        
        var oeim = vec4<u32>(sxExp, syExp, szExp, imask);
        
        node.oeim[1] = pack4xU8(oeim); // (sxExp << 24u) | (syExp << 16u) | (szExp << 8u) | (imask);

        node.ntbi[0] = childNodeBaseIndex;
        node.ntbi[1] = primitiveBaseIndex;

        bvh8Nodes[bvh8Node] = node;
    }

    // atomicStore(&debug[16u * threadId + 0u], threadId);
    // atomicStore(&debug[16u * threadId + 1u], 4u);
}

// @compute @workgroup_size(64, 1, 1)
// fn hlbvh_to_bvh8(@builtin(global_invocation_id) id: vec3<u32>) {
//     let threadId = id.x;

//     if threadId >= uniforms.num {
//         atomicStore(&debug[16u * threadId + 15u], 2u);
//         return;
//     }

    
//     // atomicStore(&debug[16u * threadId + 0u], 0u);
//     // atomicStore(&debug[16u * threadId + 1u], 0u);
//     // atomicStore(&debug[16u * threadId + 2u], 0u);
//     // atomicStore(&debug[16u * threadId + 3u], 0u);
//     // atomicStore(&debug[16u * threadId + 4u], 0u);
//     // atomicStore(&debug[16u * threadId + 5u], 0u);
//     // atomicStore(&debug[16u * threadId + 6u], 0u);
//     // atomicStore(&debug[16u * threadId + 7u], 0u);
//     // atomicStore(&debug[16u * threadId + 8u], 0u);
//     // atomicStore(&debug[16u * threadId + 9u], 0u);
//     // atomicStore(&debug[16u * threadId + 10u], 0u);
//     // atomicStore(&debug[16u * threadId + 11u], 0u);
//     // atomicStore(&debug[16u * threadId + 12u], 0u);
//     // atomicStore(&debug[16u * threadId + 13u], 0u);
//     // atomicStore(&debug[16u * threadId + 14u], 0u);
//     // atomicStore(&debug[16u * threadId + 15u], 0u);
//     var iter = 0u;

//     loop {
//         let bvh2ClusterId = indexPairs[2u * threadId + 0u];
//         let bvh8ClusterId = indexPairs[2u * threadId + 1u];

//         if bvh2ClusterId == U32_MAX || bvh8ClusterId == U32_MAX {
//             continue;
//         }

//         if nodes[bvh2ClusterId].left == nodes[bvh2ClusterId].right {
//             // let triId = nodes[bvh2ClusterId].left;
//             // let v0 = vertices[(triangles[triId].vertice0)];
//             // let v1 = vertices[(triangles[triId].vertice1)];
//             // let v2 = vertices[(triangles[triId].vertice2)];
//             // ouTtriangles[(3 * bvh8ClusterId) + 0] = vertices[(triangles[triId].vertice0)];
//             // ouTtriangles[(3 * bvh8ClusterId) + 1] = vertices[(triangles[triId].vertice1)];
//             // ouTtriangles[(3 * bvh8ClusterId) + 2] = vertices[(triangles[triId].vertice2)];
//             // atomicAdd(&debug[16u * bvh8ClusterId + 0u], bvh8ClusterId);
//             atomicStore(&debug[16u * threadId + 13u], iter);
//             atomicStore(&debug[16u * threadId + 15u], 1u);
//             atomicStore(&debug[16u * threadId + 14u], uniforms.num);

//             return;
//         }


//         var childs = array<i32, 8>(-1, -1, -1, -1, -1, -1, -1, -1);
//         var childsCount = 0u;

//         heap_clear(&heap);

//         heap_push(&heap, nodes[bvh2ClusterId].left, get_surface_area(load_bounds(nodes[bvh2ClusterId].left)));
//         heap_push(&heap, nodes[bvh2ClusterId].right, get_surface_area(load_bounds(nodes[bvh2ClusterId].right)));

//         while heap_size(&heap) > 0 && (heap_size(&heap) + i32(childsCount)) < 8 {
//             let nodeId = heap_pop(&heap);

//             if nodes[nodeId].left == nodes[nodeId].right {
//                 childs[childsCount] = nodeId;
//                 childsCount += 1u;
//             } else {
//                 heap_push(&heap, nodes[nodeId].left, get_surface_area(load_bounds(nodes[nodeId].left)));
//                 heap_push(&heap, nodes[nodeId].right, get_surface_area(load_bounds(nodes[nodeId].right)));
//             }
//         }

//         while heap_size(&heap) > 0 {
//             let nodeId = heap_pop(&heap);

//             if nodes[nodeId].left == nodes[nodeId].right {
//                 atomicAdd(&debug[16u * threadId + 14u], 1u);
//             }

//             childs[childsCount] = nodeId;
//             childsCount += 1u;
//         }

//         var isLeaf = array<bool, 8>();
//         var internalCount = 0u;
//         var leavesCount = 0u;

//         for (var i = 0u; i < 8u; i += 1u) {
//             isLeaf[i] = false;
//         }

//         for (var i = 0u; i < childsCount; i += 1u) {
//             if nodes[(childs[i])].left == nodes[(childs[i])].right {
//                 isLeaf[i] = true;
//                 leavesCount += 1u;
//             } else {
//                 isLeaf[i] = false;
//                 internalCount += 1u;
//             }
//         }

//         var parentBounds = load_bounds(i32(bvh2ClusterId)); // mat2x3<f32>(vec3<f32>(F32_MAX, F32_MAX, F32_MAX), vec3<f32>(-F32_MAX, -F32_MAX, -F32_MAX));
//         var childBounds = array<mat2x3<f32>, 8>(); //load_bounds(i32(bvh2ClusterId)); // mat2x3<f32>(vec3<f32>(F32_MAX, F32_MAX, F32_MAX), vec3<f32>(-F32_MAX, -F32_MAX, -F32_MAX));

//         for (var i = 0u; i < 8u; i++) {
//             if i >= childsCount {
//                 continue;
//             } else {
//                 childBounds[i] = load_bounds(childs[i]);
//             }
//         }

//         var cost = array<array<f32, 8>, 8>();
//         var assignments = array<i32, 8>(-1, -1, -1, -1, -1, -1, -1, -1);
//         var isSlotEmpty = array<bool, 8>(true, true, true, true, true, true, true, true);

//         let parentCentroid = (parentBounds[0] + parentBounds[1]) * 0.5;

//         for (var s = 0; s < 8; s++) {
//             var ds = vec3<f32>();

//             ds.x = select(1.0, -1.0, (s & 4) == 1);
//             ds.y = select(1.0, -1.0, (s & 2) == 1);
//             ds.z = select(1.0, -1.0, (s & 1) == 1);

//             for (var childIndex = 0u; childIndex < 8u; childIndex++) {
//                 if childs[childIndex] == -1 {
//                     cost[s][childIndex] = F32_MAX;
//                 } else {
//                     let childCentroid = (childBounds[childIndex][0] + childBounds[childIndex][1]) * 0.5;
//                     cost[s][childIndex] = dot(childCentroid - parentCentroid, ds);
//                 }
//             }
//         }

//         // Greedy ordering, TODO: add sort
//         while true {
//             var minCost = F32_MAX;
//             var minEntry = vec2<i32>(-1, -1);
//             for (var s = 0; s < 8; s++) {
//                 for (var childIndex = 0; childIndex < 8; childIndex++) {
//                     if assignments[childIndex] == -1 && isSlotEmpty[s] && cost[s][childIndex] < minCost {
//                         minCost = cost[s][childIndex];
//                         minEntry = vec2<i32>(s, childIndex);
//                     }
//                 }
//             }

//             if minEntry.x == -1 && minEntry.y == -1 {
//                 break;
//             }

//             isSlotEmpty[minEntry.x] = false;
//             assignments[minEntry.y] = minEntry.x;
//         }

//         for (var childIndex = 0; childIndex < 8; childIndex++) {
//             if assignments[childIndex] == -1 {
//                 for (var s = 0; s < 8; s++) {
//                     if isSlotEmpty[s] {
//                         isSlotEmpty[s] = false;
//                         assignments[childIndex] = s;
//                         break;
//                     }
//                 }
//             }
//         }

//         var orderedChilds = array<i32, 8>();
//         var orderedBounds = array<mat2x3<f32>, 8>();
//         var orderedLeafs = array<bool, 8>();

//         for (var i = 0; i < 8; i++) {
//             orderedChilds[(assignments[i])] = childs[i];
//             orderedBounds[(assignments[i])] = childBounds[i];
//             orderedLeafs[(assignments[i])] = isLeaf[i];
//         }

//         var node = BVH8Node();

//         node.info[0] = 0u;
//         node.info[1] = 0u;

//         node.qlox[0] = 0u;
//         node.qlox[1] = 0u;
//         node.qloy[0] = 0u;
//         node.qloy[1] = 0u;
//         node.qloz[0] = 0u;
//         node.qloz[1] = 0u;
//         node.qhix[0] = 0u;
//         node.qhix[1] = 0u;
//         node.qhiy[0] = 0u;
//         node.qhiy[1] = 0u;
//         node.qhiz[0] = 0u;
//         node.qhiz[1] = 0u;

//         var e = ceil(log2((parentBounds[1] - parentBounds[0]) / 255.0));

//         let sxExp = bitcast<u32>(i32(e.x)) & 0xFFu;
//         let syExp = bitcast<u32>(i32(e.y)) & 0xFFu;
//         let szExp = bitcast<u32>(i32(e.z)) & 0xFFu;

//         var imask = 0u;

//         var internalNodeOffset = 0u;
//         var internalLeafOffset = 0u;

//         let indexTable = array<u32, 8>(0, 0, 0, 0, 1, 1, 1, 1);
//         let offsetTable = array<u32, 8>(0, 8, 16, 24, 0, 8, 16, 24);

//         var increment = 0u;

//         if internalCount + leavesCount > 0u {
//             increment = internalCount + leavesCount - 1u;
//         }

//         let baseWorkerOffset = atomicAdd(&signals[2], increment);
//         let childNodeBaseIndex = atomicAdd(&signals[3], internalCount);
//         let primitiveBaseIndex = atomicAdd(&signals[4], leavesCount);

//         if internalCount + leavesCount > 0u {
//             var nodeOffset = 0u;
//             var primitiveOffset = 0u;

//             for (var i = 0u; i < 8u; i += 1u) {
//                 if orderedChilds[i] == -1 {
//                     continue;
//                 }

//                 let qlox = floor((orderedBounds[i][0] - parentBounds[0]).x / pow(2.0, e.x));
//                 let qloy = floor((orderedBounds[i][0] - parentBounds[0]).y / pow(2.0, e.y));
//                 let qloz = floor((orderedBounds[i][0] - parentBounds[0]).z / pow(2.0, e.z));
//                 let qhix = ceil((orderedBounds[i][1] - parentBounds[0]).x / pow(2.0, e.x));
//                 let qhiy = ceil((orderedBounds[i][1] - parentBounds[0]).y / pow(2.0, e.y));
//                 let qhiz = ceil((orderedBounds[i][1] - parentBounds[0]).z / pow(2.0, e.z));

//                 let index = indexTable[i];
//                 let offset = offsetTable[i];

//                 node.qlox[index] |= u32(qlox) << offset;
//                 node.qloy[index] |= u32(qloy) << offset;
//                 node.qloz[index] |= u32(qloz) << offset;
//                 node.qhix[index] |= u32(qhix) << offset;
//                 node.qhiy[index] |= u32(qhiy) << offset;
//                 node.qhiz[index] |= u32(qhiz) << offset;

//                 let workerAddr = select(baseWorkerOffset + nodeOffset + primitiveOffset, threadId, (nodeOffset + primitiveOffset) == 0u);

//                 atomicAdd(&debug[16u * bvh8ClusterId + 3u + i], workerAddr);

//                 if orderedLeafs[i] {
//                     var binaryEncodedPrimitiveCount = 1u;
//                     let metaField = (binaryEncodedPrimitiveCount << 5u) | internalLeafOffset;

//                     internalLeafOffset += 1u;

//                     node.info[index] |= metaField << offset;

//                     indexPairs[workerAddr * 2u + 0u] = u32(orderedChilds[i]);
//                     indexPairs[workerAddr * 2u + 1u] = primitiveBaseIndex + primitiveOffset;


//                     primitiveOffset += 1u;
//                 } else {
//                     // The index here is probably wrong, the child is not and index but a slot. Need to understand what that is,
//                     // see paper and tyni bvh https://github.com/jbikker/tinybvh/blob/main/tiny_bvh.h
//                     imask |= (1u << i);
//                     // Note that the index in the trace function if computed by bit count,  not by the index + 24 stored here.
//                     // This is rather the slot index.
//                     let metaField = (1u << 5u) | ((i + 24u) & 0x1fu);
//                     node.info[index] |= metaField << offset;

//                     indexPairs[workerAddr * 2u + 0u] = u32(orderedChilds[i]);
//                     indexPairs[workerAddr * 2u + 1u] = childNodeBaseIndex + nodeOffset;

//                     nodeOffset += 1u;
//                 }
//             }
//         }

//         atomicAdd(&debug[16u * bvh8ClusterId + 0u], bvh8ClusterId);
//         atomicAdd(&debug[16u * bvh8ClusterId + 1u], bvh2ClusterId);
//         atomicAdd(&debug[16u * bvh8ClusterId + 2u], imask);
//         atomicStore(&debug[16u * threadId + 14u], uniforms.num);

//         node.orxy[0] = bitcast<u32>(parentBounds[0].x);
//         node.orxy[1] = bitcast<u32>(parentBounds[0].y);
//         node.oeim[0] = bitcast<u32>(parentBounds[0].z);

//         node.oeim[1] = pack4xU8(vec4<u32>(sxExp, syExp, szExp, imask));// (sxExp << 24u) | (syExp << 16u) | (szExp << 8u) | (imask);

//         node.ntbi[0] = childNodeBaseIndex;
//         node.ntbi[1] = primitiveBaseIndex;


//         bvh8Nodes[bvh8ClusterId] = node;
//     }

//     atomicStore(&debug[16u * threadId + 15u], 4u);
//     atomicStore(&debug[16u * threadId + 14u], uniforms.num);
// }



// @compute @workgroup_size(64, 1, 1)
// fn hlbvh_to_bvh8(@builtin(global_invocation_id) id: vec3<u32>) {
//     let threadId = id.x;

//     for (var depth = 0u; depth < 100u; depth++) {
//         var bvh2ClusterId = U32_MAX;
//         var bvh8ClusterId = U32_MAX;

//         if threadId < u32(uniforms.num) {
//             bvh2ClusterId = indexPairs[2 * threadId + 0];
//             bvh8ClusterId = indexPairs[2 * threadId + 1];
//         }

//         workgroupBarrier();

//         if bvh2ClusterId != U32_MAX && nodes[bvh2ClusterId].left == nodes[bvh2ClusterId].right {
//             let triId = nodes[bvh2ClusterId].left;
            
//             let v0 = vertices[(triangles[triId].vertice0)];
//             let v1 = vertices[(triangles[triId].vertice1)];
//             let v2 = vertices[(triangles[triId].vertice2)];

//             ouTtriangles[(3 * bvh8ClusterId) + 0] = vertices[(triangles[triId].vertice0)];
//             ouTtriangles[(3 * bvh8ClusterId) + 1] = vertices[(triangles[triId].vertice1)];
//             ouTtriangles[(3 * bvh8ClusterId) + 2] = vertices[(triangles[triId].vertice2)];

//             bvh2ClusterId = U32_MAX;

//             indexPairs[threadId * 2 + 0] = U32_MAX;
//             indexPairs[threadId * 2 + 1] = U32_MAX;
//         }

//         workgroupBarrier();
        
//         var childs = array<i32, 8>(-1, -1, -1, -1, -1, -1, -1, -1);
//         var childsCount = 0u;
//         var stack = array<i32, 8>(-1, -1, -1, -1, -1, -1, -1, -1);
//         var stackHead = 0u;
//         var isLeaf = array<bool, 8>(false, false, false, false, false, false, false, false);
//         var internalCount = 0u;
//         var leavesCount = 0u;

//         if bvh2ClusterId != U32_MAX {
//             stack[stackHead] = i32(bvh2ClusterId);
//             stackHead += 1u;
//         }

//         workgroupBarrier();

//         while stackHead > 0 && (stackHead + childsCount) < 8u {
//             stackHead -= 1;

//             let nodeId = stack[stackHead];

//             if nodes[nodeId].left == nodes[nodeId].right {
//                 childs[childsCount] = nodeId;
//                 childsCount += 1;
//             }
//             else {
//                 stack[stackHead] = nodes[nodeId].left;
//                 stackHead += 1;
            
//                 stack[stackHead] = nodes[nodeId].right;
//                 stackHead += 1;
//             }
//         }
        
//         workgroupBarrier();

//         while stackHead > 0 {
//             stackHead -= 1;
//             childs[childsCount] = stack[stackHead];
//             childsCount += 1;
//         }

//         workgroupBarrier();

//         for(var i = 0u; i < 8u; i += 1u) {
//             if i >= childsCount {
//                 continue;
//             }

//             if nodes[(childs[i])].left == nodes[(childs[i])].right {
//                 isLeaf[i] = true;
//                 leavesCount += 1u;
//             }
//             else {
//                 isLeaf[i] = false;
//                 internalCount += 1u;
//             }
//         }
    
//         workgroupBarrier();
        
//         var increment = 0u;
        
//         if internalCount + leavesCount > 0 {
//             increment = internalCount + leavesCount - 1;
//         }

//         let baseWorkerOffset   = atomicAdd(&signals[2], u32(increment));
//         let childNodeBaseIndex = atomicAdd(&signals[3], u32(internalCount));
//         let primitiveBaseIndex = atomicAdd(&signals[4], u32(leavesCount));

//         if (internalCount + leavesCount) > 0 {
//             var nodeOffset = 0u;
//             var primitiveOffset = 0u;

//             for (var i = 0u; i < childsCount; i += 1u) {
//                 let workerAddr = select(baseWorkerOffset + i, threadId, i == 0u);

//                 if isLeaf[i] {
//                     indexPairs[workerAddr * 2 + 0] = u32(childs[i]);
//                     indexPairs[workerAddr * 2 + 1] = primitiveBaseIndex + primitiveOffset;

//                     primitiveOffset += 1;
//                 }
//                 else {
//                     // atomicAdd(&debug[9 * (childNodeBaseIndex + nodeOffset) + 1], childs[i]);
//                     // atomicAdd(&debug[9 * (childNodeBaseIndex + nodeOffset) + 2], i32(workerAddr));
//                     // atomicAdd(&debug[9 * (childNodeBaseIndex + nodeOffset) + 3], 1);
                    
//                     indexPairs[workerAddr * 2 + 0] = u32(childs[i]);
//                     indexPairs[workerAddr * 2 + 1] = childNodeBaseIndex + nodeOffset;

//                     nodeOffset += 1;
//                 }
//             }
//         }

//         workgroupBarrier();

//         var parentBounds = mat2x3<f32>(vec3<f32>(F32_MAX, F32_MAX, F32_MAX), vec3<f32>(-F32_MAX, -F32_MAX, -F32_MAX)); //load_bounds(i32(bvh2ClusterId)); 
//         var childBounds = array<mat2x3<f32>, 8>();

//         if bvh2ClusterId != U32_MAX {
//             parentBounds = load_bounds(i32(bvh2ClusterId)); 
//             childBounds = array<mat2x3<f32>, 8>();

//             for(var i = 0u; i < 8u; i++) {
//                 if i >= childsCount {
//                     childBounds[i] = mat2x3<f32>(vec3<f32>(F32_MAX, F32_MAX, F32_MAX), vec3<f32>(-F32_MAX, -F32_MAX, -F32_MAX));
//                 } else {
//                     childBounds[i] = load_bounds(childs[i]);
//                 }
//             }

//             var node = BVH8Node();

//             node.info[0] = 0u;
//             node.info[1] = 0u;
            
//             node.qlox[0] = 0u;
//             node.qlox[1] = 0u;
//             node.qloy[0] = 0u;
//             node.qloy[1] = 0u;
//             node.qloz[0] = 0u;
//             node.qloz[1] = 0u;
//             node.qhix[0] = 0u;
//             node.qhix[1] = 0u;
//             node.qhiy[0] = 0u;
//             node.qhiy[1] = 0u;
//             node.qhiz[0] = 0u;
//             node.qhiz[1] = 0u;

//             var e = ceil(log2((parentBounds[1] - parentBounds[0]) / 255.0));

//             let sxExp = bitcast<u32>(i32(e.x)); 
//             let syExp = bitcast<u32>(i32(e.y)); 
//             let szExp = bitcast<u32>(i32(e.z)); 

//             var imask = 0u;

//             var internalNodeOffset = 0u;
//             var internalLeafOffset = 0u;
            
//             // atomicStore(&debug[9 * bvh8ClusterId + 4], i32(childsCount));

//             let indexTable = array<u32, 8>(0, 0, 0, 0, 1, 1, 1, 1);
//             let offsetTable = array<u32, 8>(24, 16, 8, 0, 24, 16, 8, 0);

//             var scale = vec3<f32>(1.0 / pow(2.0, e.x), 1.0 / pow(2.0, e.y), 1.0 / pow(2.0, e.z));

//             for (var i = 0u; i < 8u; i += 1u) {
//                 if i >= childsCount {
//                     continue;
//                 }

//                 let qlo = floor((childBounds[i][0] - parentBounds[0]) * scale);
//                 let qhi = ceil((childBounds[i][1] - parentBounds[0]) * scale);

//                 let index = indexTable[i];
//                 let offset = offsetTable[i];
                
//                 node.qlox[index] |= (u32(qlo.x) << offset);
//                 node.qloy[index] |= (u32(qlo.y) << offset);
//                 node.qloz[index] |= (u32(qlo.z) << offset);
//                 node.qhix[index] |= (u32(qhi.x) << offset);
//                 node.qhiy[index] |= (u32(qhi.y) << offset);
//                 node.qhiz[index] |= (u32(qhi.z) << offset);
                
//                 atomicStore(&debug[16 * bvh8ClusterId + i * 2 + 0], u32(qlo.y));
//                 atomicStore(&debug[16 * bvh8ClusterId + i * 2 + 1], u32(qhi.y));

//                 // atomicStore(&debug[16 * bvh8ClusterId + 1], u32(qloy));
//                 // atomicStore(&debug[16 * bvh8ClusterId + 2], u32(qloz));
//                 // atomicStore(&debug[16 * bvh8ClusterId + 3], u32(qhix));
//                 // atomicStore(&debug[16 * bvh8ClusterId + 4], u32(qhiy));
//                 // atomicStore(&debug[16 * bvh8ClusterId + 5], u32(qhiz));

//                 if (isLeaf[i]) {
//                     imask = imask | (0u << i);

//                     var binaryEncodedPrimitiveCount = 1u;

//                     let metaField = (binaryEncodedPrimitiveCount << 5u) | internalLeafOffset;

//                     internalLeafOffset += 1u;
                    
//                     node.info[index] |= metaField << offset;
//                 }
//                 else {
//                     imask = imask | (1u << i);

//                     let metaField = (1u << 5u) | (internalNodeOffset + 24u);
                    
//                     internalNodeOffset += 1u;

//                     node.info[index] |= metaField << offset;
//                 }
//             }
            
//             node.orxy[0] = bitcast<u32>(parentBounds[0].x);
//             node.orxy[1] = bitcast<u32>(parentBounds[0].y);
//             node.oeim[0] = bitcast<u32>(parentBounds[0].z);
        
//             node.oeim[1] = (sxExp << 24u) | (syExp << 16u) | (szExp << 8u) | (imask << 0u);

//             node.ntbi[0] = childNodeBaseIndex;
//             node.ntbi[1] = primitiveBaseIndex;

//             // atomicAdd(&debug[9 * bvh8ClusterId + 0], 1);

//             bvh8Nodes[bvh8ClusterId * 5 + 0] = vec4<u32>(node.orxy[0], node.orxy[1], node.oeim[0], node.oeim[1]);
//             bvh8Nodes[bvh8ClusterId * 5 + 1] = vec4<u32>(node.ntbi[0], node.ntbi[1], node.info[0], node.info[1]);
//             bvh8Nodes[bvh8ClusterId * 5 + 2] = vec4<u32>(node.qlox[0], node.qlox[1], node.qloy[0], node.qloy[1]);
//             bvh8Nodes[bvh8ClusterId * 5 + 3] = vec4<u32>(node.qloz[0], node.qloz[1], node.qhix[0], node.qhix[1]);
//             bvh8Nodes[bvh8ClusterId * 5 + 4] = vec4<u32>(node.qhiy[0], node.qhiy[1], node.qhiz[0], node.qhiz[1]);
//         }
//     }   
// }



// @compute @workgroup_size(64, 1, 1)
// fn bvh8_to_lines(@builtin(global_invocation_id) id: vec3<u32>) {
//     if (id.x >= u32(ceil(f32(2 * uniforms.num - 1) / 8.0))) {
//         return;
//     }

//     let n0: vec4<u32> = bvh8Nodes[id.x * 5 + 0];
//     let n1: vec4<u32> = bvh8Nodes[id.x * 5 + 1];
//     let n2: vec4<u32> = bvh8Nodes[id.x * 5 + 2];
//     let n3: vec4<u32> = bvh8Nodes[id.x * 5 + 3];
//     let n4: vec4<u32> = bvh8Nodes[id.x * 5 + 4];

//     let p: vec3<f32> = vec3<f32>(
//         bitcast<f32>(n0.x),
//         bitcast<f32>(n0.y),
//         bitcast<f32>(n0.z)
//     );

//     var oeim = bitcast<u32>(n0.w);                
//     var e: vec3<u32> = vec3<u32>(
//         bitcast<u32>(oeim >> 24) & 0xFF,
//         bitcast<u32>(oeim >> 16) & 0xFF,
//         bitcast<u32>(oeim >>  8) & 0xFF,
//     );

//     let sxExp = (oeim >> 24u) & 0xFFu;
//     let syExp = (oeim >> 16u) & 0xFFu;
//     let szExp = (oeim >> 8u) & 0xFFu;

//     let scaleX = pow(2.0, f32(sxExp));
//     let scaleY = pow(2.0, f32(syExp));
//     let scaleZ = pow(2.0, f32(szExp));

//     // Step 3: Reconstruct diag as scale factors multiplied by 255 (quantization range)
//     let diag = vec3<f32>(
//         scaleX * 255.0,
//         scaleY * 255.0,
//         scaleZ * 255.0
//     );


//     var ex = bitcast<f32>((e.x + 127) << 23) * (pow(2.0, 8.0) - 1.0);
//     var ey = bitcast<f32>((e.y + 127) << 23) * (pow(2.0, 8.0) - 1.0);
//     var ez = bitcast<f32>((e.z + 127) << 23) * (pow(2.0, 8.0) - 1.0);
//     // let ex = bitcast<f32>((e[0] + 127) << 23);
//     // let ey = bitcast<f32>((e[1] + 127) << 23);
//     // let ez = bitcast<f32>((e[2] + 127) << 23);

//     {
//         let minPoint = p;
//         let maxPoint = p + diag;

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
//         let baseIndex = id.x * 24 * 3; // 12 lines * 2 points * 3 components per point

//         for (var i = 0u; i < 12u; i = i + 1) {
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





// @compute @workgroup_size(64, 1, 1)
// fn hlbvh_to_bvh8(@builtin(global_invocation_id) id: vec3<u32>) {
//    // var failsafe = 10000;
//     var bvh2ClusterId = U32_MAX;
//     var bvh8ClusterId = U32_MAX;

//     let threadId = id.x;
    
//     if threadId >= u32(uniforms.num) {
//         return;
//     }

//     loop {
//         let signal = atomicLoad(&signals[5]);
        
//         if signal != 0 {
//             return;
//         }

//         bvh2ClusterId = indexPairs[2 * threadId + 0];
//         bvh8ClusterId = indexPairs[2 * threadId + 1];

//         if bvh2ClusterId == U32_MAX {
//             continue;
//         }
        
//         if  nodes[bvh2ClusterId].left == nodes[bvh2ClusterId].right { 
//             let triId = nodes[bvh2ClusterId].left;
            
//             let v0 = vertices[(triangles[triId].vertice0)];
//             let v1 = vertices[(triangles[triId].vertice1)];
//             let v2 = vertices[(triangles[triId].vertice2)];

//             let woopTriangle = computeWoopCoordinates(v0, v1, v2);

//             bvh8Nodes[5 * u32(ceil(f32(2 * uniforms.num - 1) / 8.0)) + 3 * bvh8ClusterId + 0] = bitcast<vec4<u32>>(woopTriangle[0]);
//             bvh8Nodes[5 * u32(ceil(f32(2 * uniforms.num - 1) / 8.0)) + 3 * bvh8ClusterId + 1] = bitcast<vec4<u32>>(woopTriangle[1]);
//             bvh8Nodes[5 * u32(ceil(f32(2 * uniforms.num - 1) / 8.0)) + 3 * bvh8ClusterId + 2] = bitcast<vec4<u32>>(woopTriangle[2]);
            
//             return;
//         }

//         var children = ChildStack();

//         var bvh2Node = nodes[bvh2ClusterId];
//         var bounds = array<mat2x3<f32>, 2>();
//         var childClusters = vec2<u32>(u32(bvh2Node.left), u32(bvh2Node.right));

//         bounds[0] = load_bounds(i32(childClusters.x));
//         bounds[1] = load_bounds(i32(childClusters.y));
        
//         let surface_area = array<f32,2>(get_surface_area(bounds[0]), get_surface_area(bounds[1]));
        
//         var order = array<u32, 2>();

//         order[0] = select(1u, 0u, surface_area[0] > surface_area[1]);
//         order[1] = select(0u, 1u, surface_area[0] > surface_area[1]);

//         // Maybe istead of stack use inserion sort to pick the bigger one
//         child_stack_push_cluster(&children, childClusters[(order[0])]);
//         child_stack_push_cluster(&children, childClusters[(order[1])]);

//         while(child_stack_can_open_inner_node(&children)) {
//             let nodeId = child_stack_pop(&children);
//             var innerNode = nodes[nodeId];
//             var bounds = array<mat2x3<f32>, 2>();
//             var childClusters = vec2<u32>(u32(innerNode.left), u32(innerNode.right));

//             bounds[0] = load_bounds(i32(childClusters.x));
//             bounds[1] = load_bounds(i32(childClusters.y));
            
//             let surface_area = array<f32,2>(get_surface_area(bounds[0]), get_surface_area(bounds[1]));
//             var order = array<u32, 2>();

//             order[0] = select(1u, 0u, surface_area[0] > surface_area[1]);
//             order[1] = select(0u, 1u, surface_area[0] > surface_area[1]);
            
//             child_stack_push_cluster(&children, childClusters[(order[0])]);
//             child_stack_push_cluster(&children, childClusters[(order[1])]); 
//         }

//         let baseWorkerOffset = atomicAdd(&signals[2], (children.numNodes + children.numLeaves) - 1u);
//         let childNodeBaseIndex = atomicAdd(&signals[3], children.numNodes);
//         let primitiveBaseIndex = atomicAdd(&signals[4], children.numLeaves);

//         if childNodeBaseIndex + children.numNodes > arrayLength(&bvh8Nodes) {
//             atomicStore(&signals[5], ERROR_OUT_OF_BOUNDS);

//             // bvh8Nodes[0].aabbMaxX = f32(atomicLoad(&signals[0]));
//             // bvh8Nodes[0].aabbMaxY = f32(atomicLoad(&signals[1]));
//             // bvh8Nodes[0].aabbMaxZ = f32(atomicLoad(&signals[2]));
//             // bvh8Nodes[0].aabbMinX = f32(atomicLoad(&signals[3]));
//             // bvh8Nodes[0].aabbMinY = f32(atomicLoad(&signals[4]));
//             // bvh8Nodes[0].aabbMinZ = f32(2);
//             return;
//         }

//         for(var i = 0u; i < (children.numLeaves + children.numNodes); i += 1u) {
//             var j = i - children.numLeaves;
//             let workerAddr = select(baseWorkerOffset + i, threadId, i == 0u);
//             var pair = array<u32, 2>();

//             if i < children.numLeaves {
//                 pair[0] = children.leaves[i];
//                 pair[1] = primitiveBaseIndex + i;
//             }
//             else {
//                 pair[0] = children.nodes[j];
//                 pair[1] = childNodeBaseIndex + j;
//             }

//             indexPairs[workerAddr * 2 + 0] = pair[0];
//             indexPairs[workerAddr * 2 + 1] = pair[1];
//         }

//         var parentBounds = mat2x3<f32>(vec3<f32>(F32_MAX, F32_MAX, F32_MAX), vec3<f32>(-F32_MAX, -F32_MAX, -F32_MAX));
//         var childBounds = array<mat2x3<f32>, 8>();

//         for (var i = 0u; i < child_stack_size(&children); i += 1u) {
//             var clusterID = select(children.nodes[i - children.numLeaves], children.leaves[i], i < children.numLeaves);
            
//             childBounds[i] = load_bounds(i32(clusterID));

//             parentBounds[0] = min(parentBounds[0], childBounds[i][0]);
//             parentBounds[1] = max(parentBounds[1], childBounds[i][1]);
//         }
        
//         var payoffs = array<array<f32, 8>, 8>();
//         var assignments = array<i32, 8>();
//         var prices = array<f32, 8>();

//         for (var i = 0u; i < 8u; i++) {
//             assignments[i] = -1;
//             prices[i] = 0.0;
//         }

//         let parentCentroid = (parentBounds[0] + parentBounds[1]) * 0.5;

//         for (var c = 0u; c < child_stack_size(&children); c++) {
//             let childCentroid = (childBounds[c][0] + childBounds[c][1]) * 0.5;
//             for(var s = 0u; s < 8u; s++) {
//                 var DS = vec3<f32>();
//                 DS.x = select(-1.0, 1.0, (s & 1) == 0); 
//                 DS.y = select(-1.0, 1.0, (s & 2) == 0); 
//                 DS.z = select(-1.0, 1.0, (s & 4) == 0); 
//                 payoffs[c][s] = dot(childCentroid - parentCentroid, DS);
//             }
//         }

//         let epsilon = 1.0 / f32(child_stack_size(&children));
        
//         var bidders = array<i32, 8u>();
//         var head = child_stack_size(&children);

//         for (var i = 0; i < i32(head); i++) {
//             bidders[i] = i;
//         }

//         while head > 0 {
//             head -= 1;
//             let c = bidders[head];

//             var winningReward = -F32_MAX;
//             var secondWinningReward = -F32_MAX;
//             var winningSlot = -1;
//             var secondWinningSlot = -1;

//             for (var s = 0; s < 8; s++) {
//                 let reward = payoffs[c][s] - prices[s];
//                 if reward > winningReward {
//                     winningReward = reward;
//                     secondWinningReward = winningReward;
//                     winningSlot = s;
//                     secondWinningSlot = winningSlot;
//                 }
//                 else if reward > secondWinningReward {
//                     secondWinningReward = reward;
//                     secondWinningSlot = s;
//                 }
//             }

//             prices[winningSlot] += (winningReward - secondWinningReward) + epsilon;
//             let previousAssignment = assignments[winningSlot];
//             assignments[winningSlot] = c;

//             if previousAssignment != -1 {
//                 bidders[head] = previousAssignment;
//                 head += 1;
//             }
//         }

//         var node = BVH8Node();
//         let denom = 1.0 / (pow(2.0, 8.0) - 1.0);
//         // Quantize to save memory and reduce bandwidth requirements
//         let origin = parentBounds[0];
//         let diag = parentBounds[1] - parentBounds[0];

//         let ex = i32(ceil(log2(diag.x * denom)));
//         let ey = i32(ceil(log2(diag.y * denom)));
//         let ez = i32(ceil(log2(diag.z * denom)));
        
//         let one_over_e = vec3<f32>(1.0 / pow(2.0, f32(ex)), 1.0 / pow(2.0, f32(ey)), 1.0 / pow(2.0, f32(ez)));

//         let sxExp = bitcast<u32>((ex) & 0xFF); //bitcast<u32>(float_to_exponent(diag.x));
//         let syExp = bitcast<u32>((ey) & 0xFF); //bitcast<u32>(float_to_exponent(diag.y));
//         let szExp = bitcast<u32>((ez) & 0xFF); //bitcast<u32>(float_to_exponent(diag.z));

//         let iMask = get_imask(assignments, children.numLeaves);

//         // node.aabbMinX = parentBounds[0].x;
//         // node.aabbMinY = parentBounds[0].y;
//         // node.aabbMinZ = parentBounds[0].z;
//         // node.aabbMaxX = parentBounds[1].x;
//         // node.aabbMaxY = parentBounds[1].y;
//         // node.aabbMaxZ = parentBounds[1].z;

//         node.orxy[0] = bitcast<u32>(origin.x);
//         node.orxy[1] = bitcast<u32>(origin.y);
//         node.oeim[0] = bitcast<u32>(origin.z);
//         node.oeim[1] = (sxExp << 24u) | (syExp << 16u) | (szExp << 8u) | ((iMask) << 0u);
//         node.ntbi[0] = childNodeBaseIndex;
//         node.ntbi[1] = primitiveBaseIndex;

//         node.info[0] = 0u;
//         node.info[1] = 0u;
//         node.qlox[0] = 0u;
//         node.qlox[1] = 0u;
//         node.qloy[0] = 0u;
//         node.qloy[1] = 0u;
//         node.qloz[0] = 0u;
//         node.qloz[1] = 0u;
//         node.qhix[0] = 0u;
//         node.qhix[1] = 0u;
//         node.qhiy[0] = 0u;
//         node.qhiy[1] = 0u;
//         node.qhiz[0] = 0u;
//         node.qhiz[1] = 0u;

//         for (var i = 0u; i < 8u; i++) {
//             let child = assignments[i];
//             if child == -1 {
//                  continue; // empty slot
//             }
//             else if (child < i32(children.numLeaves)) {
//                 // leaf node
//                 //  high 3 bits unary encode num prims 
//                 //  low 5 bits store child slot index (ranging in 0...23)
//                 // (todo... store up to 24 triangles, 3 per child slot...)
//                 let meta0 = 0u;
//                 let meta1 = (1u << 5u) | u32(child);
                
//                 var meta0r = 0u;
//                 var meta1r = 0u;
                
//                 bitwise_shift_left_u64(meta0, meta1, i * 8, &meta0r, &meta1r);
                
//                 node.info[0] |= meta0r;
//                 node.info[1] |= meta1r;
                
//             }
//             else {
//                 // inner node
//                 //   high 3 bits of info are 001.
//                 //   low 5 bits store child slot index + 24.
//                 let meta0 = 0u;
//                 let meta1 = (1u << 5u) | u32((u32(child) - children.numLeaves) + 24u);

//                 var meta0r = 0u;
//                 var meta1r = 0u;
                
//                 bitwise_shift_left_u64(meta0, meta1, i * 8, &meta0r, &meta1r);
                
//                 node.info[0] |= meta0r;
//                 node.info[1] |= meta1r;
//             }
            
//             // Quantize child AABBs and fill out the info fields

//             // The child node's box relative to the parent
//             let qlo = floor(childBounds[child][0] - origin) * one_over_e;
//             let qhi = ceil(childBounds[child][1] - origin) * one_over_e;

//             // Apply floor and ceil to ensure conservative quantization
//             let childQLo = vec3<u32>(
//                 u32(min(floor(qlo.x), 255.0)),
//                 u32(min(floor(qlo.y), 255.0)),
//                 u32(min(floor(qlo.z), 255.0))
//             );

//             let childQHi = vec3<u32>(
//                 u32(min(ceil(qhi.x), 255.0)),
//                 u32(min(ceil(qhi.y), 255.0)),
//                 u32(min(ceil(qhi.z), 255.0))
//             );


//             let index = (i * 8u) >> 5u;
//             let offst = (i * 8u) &  0x1Fu;
            
//             node.qlox[index] |= childQLo.x << offst;
//             node.qloy[index] |= childQLo.y << offst;
//             node.qloz[index] |= childQLo.z << offst;
//             node.qhix[index] |= childQHi.x << offst;
//             node.qhiy[index] |= childQHi.y << offst;
//             node.qhiz[index] |= childQHi.z << offst;
//         }

//         bvh8Nodes[bvh8ClusterId * 5 + 0] = vec4<u32>((node.orxy[0]), (node.orxy[1]), (node.oeim[0]), (node.oeim[1]));
//         bvh8Nodes[bvh8ClusterId * 5 + 1] = vec4<u32>((node.ntbi[0]), (node.ntbi[1]), (node.info[0]), (node.info[1]));
//         bvh8Nodes[bvh8ClusterId * 5 + 2] = vec4<u32>((node.qlox[0]), (node.qlox[1]), (node.qloy[0]), (node.qloy[1]));
//         bvh8Nodes[bvh8ClusterId * 5 + 3] = vec4<u32>((node.qloz[0]), (node.qloz[1]), (node.qhix[0]), (node.qhix[1]));
//         bvh8Nodes[bvh8ClusterId * 5 + 4] = vec4<u32>((node.qhiy[0]), (node.qhiy[1]), (node.qhiz[0]), (node.qhiz[1]));
//     }
// }