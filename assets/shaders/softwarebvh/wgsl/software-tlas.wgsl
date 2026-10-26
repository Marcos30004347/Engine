const INVALID_INDEX = 0xffffffffu;
const NODE_FLAG_LEAF = 1u;

struct InstanceAABBGPU {
    minPoint: vec3<f32>,
    instanceIndex: u32,
    maxPoint: vec3<f32>,
    blasHandle: u32,
};

struct CanonicalNodeGPU {
    parent: u32,
    left: u32,
    right: u32,
    instanceIndex: u32,
    minPoint: vec3<f32>,
    flags: u32,
    maxPoint: vec3<f32>,
    subtreeLeafCount: u32,
};

struct CWBVHNodeGPU {
    orxy: vec2<u32>,
    oeim: vec2<u32>,
    ntbi: vec2<u32>,
    info: vec2<u32>,
    qlox: vec2<u32>,
    qloy: vec2<u32>,
    qloz: vec2<u32>,
    qhix: vec2<u32>,
    qhiy: vec2<u32>,
    qhiz: vec2<u32>,
};

struct CWBVHNodeRefGPU {
    canonicalNodeIndex: u32,
    childCanonicalIndices: array<u32, 8>,
};

struct CountersGPU {
    instanceCount: u32,
    nodeCount: u32,
    rootNodeIndex: u32,
    buildRevision: u32,
    refitRevision: u32,
    dirtyInstanceCount: u32,
    buildRequested: u32,
    refitRequested: u32,
};

struct Uniforms {
    canonicalNodeCount: u32,
    cwbvhNodeCount: u32,
    instanceCount: u32,
    maxInstanceCount: u32,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var<storage, read> instanceAabbs: array<InstanceAABBGPU>;
@group(0) @binding(2) var<storage, read_write> canonicalNodes: array<CanonicalNodeGPU>;
@group(0) @binding(3) var<storage, read_write> cwbvhNodes: array<CWBVHNodeGPU>;
@group(0) @binding(4) var<storage, read_write> cwbvhNodeRefs: array<CWBVHNodeRefGPU>;
@group(0) @binding(5) var<storage, read> counters: CountersGPU;
@group(0) @binding(6) var<storage, read_write> mortonCodes: array<u32>;
@group(0) @binding(7) var<storage, read_write> instanceIndices: array<u32>;
@group(0) @binding(8) var<storage, read_write> buildFlags: array<atomic<u32>>;
@group(0) @binding(9) var<storage, read_write> buildSignals: array<atomic<u32>>;
@group(0) @binding(10) var<storage, read_write> leafInstanceIndices: array<u32>;
@group(0) @binding(11) var<storage, read_write> buildDebug: array<atomic<u32>>;

fn pack4xU8(x: u32, y: u32, z: u32, w: u32) -> u32 {
    return (x & 0xffu) | ((y & 0xffu) << 8u) | ((z & 0xffu) << 16u) | ((w & 0xffu) << 24u);
}

fn decode_signed_u8(value: u32) -> i32 {
    return select(i32(value), i32(value) - 256, value >= 128u);
}

fn encode_signed_exponent8(extent: f32) -> u32 {
    if !(extent > 0.0) {
        return 0u;
    }

    var exponent = clamp(i32(ceil(log2(extent / 255.0))), -128, 127);
    let scale = exp2(f32(exponent));
    if exponent < 127 && ceil(extent / scale) > 255.0 {
        exponent += 1;
    }
    return select(u32(exponent), u32(exponent + 256), exponent < 0) & 0xffu;
}

fn decode_exponent_scale(encodedExponent: u32) -> f32 {
    return exp2(f32(decode_signed_u8(encodedExponent & 0xffu)));
}

fn quantize_lower_bound(value: f32, parentMin: f32, scale: f32) -> u32 {
    if !(scale > 0.0) {
        return 0u;
    }
    var q = u32(clamp(floor((value - parentMin) / scale), 0.0, 255.0));
    if q > 0u {
        q -= 1u;
    }
    return q;
}

fn quantize_upper_bound(value: f32, parentMin: f32, scale: f32) -> u32 {
    if !(scale > 0.0) {
        return 0u;
    }
    var q = u32(clamp(ceil((value - parentMin) / scale), 0.0, 255.0));
    if q < 255u {
        q += 1u;
    }
    return q;
}

fn expand_bits(value: u32) -> u32 {
    var v = value & 0x3ffu;
    v = (v | (v << 16u)) & 0x030000ffu;
    v = (v | (v << 8u)) & 0x0300f00fu;
    v = (v | (v << 4u)) & 0x030c30c3u;
    v = (v | (v << 2u)) & 0x09249249u;
    return v;
}

fn morton3(normalizedPosition: vec3<f32>) -> u32 {
    let p = clamp(normalizedPosition, vec3<f32>(0.0), vec3<f32>(0.999999));
    let x = u32(clamp(p.x * 1024.0, 0.0, 1023.0));
    let y = u32(clamp(p.y * 1024.0, 0.0, 1023.0));
    let z = u32(clamp(p.z * 1024.0, 0.0, 1023.0));
    return (expand_bits(x) << 2u) | (expand_bits(y) << 1u) | expand_bits(z);
}

fn safe_normalized_centroid(centroid: vec3<f32>, rootMin: vec3<f32>, rootMax: vec3<f32>) -> vec3<f32> {
    let extent = rootMax - rootMin;
    return vec3<f32>(
        select(0.5, (centroid.x - rootMin.x) / extent.x, extent.x > 0.0),
        select(0.5, (centroid.y - rootMin.y) / extent.y, extent.y > 0.0),
        select(0.5, (centroid.z - rootMin.z) / extent.z, extent.z > 0.0));
}

fn hlbvh_leaf_index(leafIndex: u32) -> u32 {
    return uniforms.instanceCount - 1u + leafIndex;
}

fn hlbvh_common_prefix_length(i1: i32, i2: i32) -> i32 {
    let left = min(i1, i2);
    let right = max(i1, i2);

    if left < 0 || right >= i32(uniforms.instanceCount) {
        return 0;
    }

    let leftCode = mortonCodes[u32(left)];
    let rightCode = mortonCodes[u32(right)];
    if leftCode != rightCode {
        return i32(countLeadingZeros(leftCode ^ rightCode));
    }

    return 32 + i32(countLeadingZeros(u32(left ^ right)));
}

fn hlbvh_find_span(index: i32) -> vec2<u32> {
    let d = sign(hlbvh_common_prefix_length(index, index + 1) - hlbvh_common_prefix_length(index, index - 1));
    let deltaMin = hlbvh_common_prefix_length(index, index - d);

    var lmax = 2;
    while hlbvh_common_prefix_length(index, index + lmax * d) > deltaMin {
        lmax *= 2;
    }

    var l = 0;
    var t = lmax;
    while t > 1 {
        t /= 2;
        if hlbvh_common_prefix_length(index, index + (l + t) * d) > deltaMin {
            l += t;
        }
    }

    let first = u32(min(index, index + l * d));
    let last = u32(max(index, index + l * d));
    return vec2<u32>(first, min(last, uniforms.instanceCount - 1u));
}

fn hlbvh_find_split(span: vec2<u32>) -> u32 {
    var left = i32(span.x);
    var right = i32(span.y);
    let commonPrefix = hlbvh_common_prefix_length(left, right);

    while right > left + 1 {
        let split = (left + right) / 2;
        if hlbvh_common_prefix_length(left, split) > commonPrefix {
            left = split;
        } else {
            right = split;
        }
    }

    return u32(left);
}

fn canonical_surface_area(nodeIndex: u32) -> f32 {
    let node = canonicalNodes[nodeIndex];
    let d = max(node.maxPoint - node.minPoint, vec3<f32>(0.0));
    return max(2.0 * (d.x * d.y + d.x * d.z + d.y * d.z), 0.0);
}

struct Pair {
    key: u32,
    value: f32,
};

struct MaxHeap {
    data: array<Pair, 8>,
    count: i32,
};

var<private> heap: MaxHeap;
var<private> indexTable = array<u32, 8>(0u, 0u, 0u, 0u, 1u, 1u, 1u, 1u);
var<private> offsetTable = array<u32, 8>(0u, 8u, 16u, 24u, 0u, 8u, 16u, 24u);

fn heap_clear(heapPtr: ptr<private, MaxHeap>) {
    (*heapPtr).count = 0;
}

fn heap_size(heapPtr: ptr<private, MaxHeap>) -> i32 {
    return (*heapPtr).count;
}

fn heap_push(heapPtr: ptr<private, MaxHeap>, key: u32, value: f32) {
    var current = (*heapPtr).count;
    if current >= 8 {
        return;
    }

    (*heapPtr).data[current].key = key;
    (*heapPtr).data[current].value = value;
    (*heapPtr).count += 1;

    while current > 0 {
        let parent = (current - 1) / 2;
        if (*heapPtr).data[parent].value >= (*heapPtr).data[current].value {
            break;
        }
        let temp = (*heapPtr).data[parent];
        (*heapPtr).data[parent] = (*heapPtr).data[current];
        (*heapPtr).data[current] = temp;
        current = parent;
    }
}

fn heap_pop(heapPtr: ptr<private, MaxHeap>) -> u32 {
    if (*heapPtr).count <= 0 {
        return INVALID_INDEX;
    }

    let result = (*heapPtr).data[0].key;
    (*heapPtr).count -= 1;
    (*heapPtr).data[0] = (*heapPtr).data[(*heapPtr).count];

    var current = 0;
    loop {
        let left = current * 2 + 1;
        let right = current * 2 + 2;
        var largest = current;

        if left < (*heapPtr).count && (*heapPtr).data[left].value > (*heapPtr).data[largest].value {
            largest = left;
        }
        if right < (*heapPtr).count && (*heapPtr).data[right].value > (*heapPtr).data[largest].value {
            largest = right;
        }
        if largest == current {
            break;
        }

        let temp = (*heapPtr).data[current];
        (*heapPtr).data[current] = (*heapPtr).data[largest];
        (*heapPtr).data[largest] = temp;
        current = largest;
    }

    return result;
}

fn is_canonical_leaf(nodeIndex: u32) -> bool {
    return (canonicalNodes[nodeIndex].flags & NODE_FLAG_LEAF) != 0u;
}

fn make_invalid_node_ref() -> CWBVHNodeRefGPU {
    var nodeRef: CWBVHNodeRefGPU;
    nodeRef.canonicalNodeIndex = INVALID_INDEX;
    for (var i = 0u; i < 8u; i += 1u) {
        nodeRef.childCanonicalIndices[i] = INVALID_INDEX;
    }
    return nodeRef;
}

@compute @workgroup_size(64, 1, 1)
fn prepare_rebuild(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let index = globalId.x;
    if index >= uniforms.maxInstanceCount {
        return;
    }

    if counters.buildRequested == 0u {
        return;
    }

    if index < uniforms.instanceCount {
        let root = canonicalNodes[0u];
        let aabb = instanceAabbs[index];
        let centroid = (aabb.minPoint + aabb.maxPoint) * 0.5;
        mortonCodes[index] = morton3(safe_normalized_centroid(centroid, root.minPoint, root.maxPoint));
        instanceIndices[index] = index;
    }

    if index >= uniforms.instanceCount {
        mortonCodes[index] = 0xffffffffu;
        instanceIndices[index] = index;
    }
}

@compute @workgroup_size(64, 1, 1)
fn clear_build_flags(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let index = globalId.x;
    if counters.buildRequested == 0u || index >= uniforms.instanceCount {
        return;
    }

    atomicStore(&buildFlags[index * 2u + 0u], 0u);
    atomicStore(&buildFlags[index * 2u + 1u], 0u);
}

@compute @workgroup_size(64, 1, 1)
fn build_hierarchy_leaves(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let index = globalId.x;
    if counters.buildRequested == 0u || uniforms.instanceCount == 0u || index >= uniforms.instanceCount {
        return;
    }

    let sortedInstanceIndex = instanceIndices[index];
    let leafIndex = hlbvh_leaf_index(index);
    let aabb = instanceAabbs[sortedInstanceIndex];

    canonicalNodes[leafIndex].parent = INVALID_INDEX;
    canonicalNodes[leafIndex].left = sortedInstanceIndex;
    canonicalNodes[leafIndex].right = sortedInstanceIndex;
    canonicalNodes[leafIndex].instanceIndex = sortedInstanceIndex;
    canonicalNodes[leafIndex].minPoint = aabb.minPoint;
    canonicalNodes[leafIndex].flags = NODE_FLAG_LEAF;
    canonicalNodes[leafIndex].maxPoint = aabb.maxPoint;
    canonicalNodes[leafIndex].subtreeLeafCount = 1u;

    if uniforms.instanceCount == 1u {
        canonicalNodes[0u] = canonicalNodes[leafIndex];
        canonicalNodes[0u].parent = INVALID_INDEX;
        return;
    }
}

@compute @workgroup_size(64, 1, 1)
fn build_hierarchy(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let index = globalId.x;
    if counters.buildRequested == 0u || uniforms.instanceCount <= 1u || index >= uniforms.instanceCount - 1u {
        return;
    }

    let span = hlbvh_find_span(i32(index));
    let split = hlbvh_find_split(span);
    let leftChild = select(split, hlbvh_leaf_index(split), split == span.x);
    let rightChild = select(split + 1u, hlbvh_leaf_index(split + 1u), split + 1u == span.y);

    canonicalNodes[index].parent = INVALID_INDEX;
    canonicalNodes[index].left = leftChild;
    canonicalNodes[index].right = rightChild;
    canonicalNodes[index].instanceIndex = INVALID_INDEX;
    canonicalNodes[index].flags = 0u;
    canonicalNodes[index].subtreeLeafCount = 0u;

    canonicalNodes[leftChild].parent = index;
    canonicalNodes[rightChild].parent = index;
}

@compute @workgroup_size(64, 1, 1)
fn refit_hierarchy(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let index = globalId.x;
    if counters.buildRequested == 0u || uniforms.instanceCount <= 1u || index >= uniforms.instanceCount {
        return;
    }

    var nodeIndex = hlbvh_leaf_index(index);
    loop {
        nodeIndex = canonicalNodes[nodeIndex].parent;
        if atomicAdd(&buildFlags[nodeIndex], 1u) == 1u {
            let left = canonicalNodes[nodeIndex].left;
            let right = canonicalNodes[nodeIndex].right;
            canonicalNodes[nodeIndex].minPoint = min(canonicalNodes[left].minPoint, canonicalNodes[right].minPoint);
            canonicalNodes[nodeIndex].maxPoint = max(canonicalNodes[left].maxPoint, canonicalNodes[right].maxPoint);
            canonicalNodes[nodeIndex].subtreeLeafCount = canonicalNodes[left].subtreeLeafCount + canonicalNodes[right].subtreeLeafCount;
        } else {
            break;
        }

        if nodeIndex == 0u {
            break;
        }
    }
}

@compute @workgroup_size(64, 1, 1)
fn initialize_cwbvh_rebuild(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let index = globalId.x;
    if counters.buildRequested == 0u || uniforms.instanceCount == 0u || index >= uniforms.instanceCount {
        return;
    }

    if index == 0u {
        atomicStore(&buildFlags[0u], 0u);
        atomicStore(&buildFlags[1u], 0u);
        atomicStore(&buildSignals[0u], 0u);
        atomicStore(&buildSignals[1u], 0u);
        atomicStore(&buildSignals[2u], 0u);
        atomicStore(&buildSignals[3u], 1u);
        atomicStore(&buildSignals[4u], 0u);
        atomicStore(&buildSignals[5u], 0u);
    } else {
        atomicStore(&buildFlags[index * 2u + 0u], INVALID_INDEX);
        atomicStore(&buildFlags[index * 2u + 1u], INVALID_INDEX);
    }
}

@compute @workgroup_size(64, 1, 1)
fn build_cwbvh_rebuild(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let threadId = globalId.x;
    if counters.buildRequested == 0u || uniforms.instanceCount == 0u || threadId >= uniforms.instanceCount {
        return;
    }

    loop {
        if atomicLoad(&buildSignals[4u]) >= uniforms.instanceCount && atomicLoad(&buildSignals[5u]) >= atomicLoad(&buildSignals[3u]) {
            atomicStore(&buildDebug[threadId * 8u + 7u], 3u);
            return;
        }

        let bvh2Node = atomicLoad(&buildFlags[threadId * 2u + 0u]);
        let bvh8Node = atomicLoad(&buildFlags[threadId * 2u + 1u]);
        if bvh2Node == INVALID_INDEX || bvh8Node == INVALID_INDEX {
            continue;
        }

        var children = array<u32, 8>(INVALID_INDEX, INVALID_INDEX, INVALID_INDEX, INVALID_INDEX, INVALID_INDEX, INVALID_INDEX, INVALID_INDEX, INVALID_INDEX);
        var head = 0u;

        heap_clear(&heap);
        if is_canonical_leaf(bvh2Node) {
            children[0u] = bvh2Node;
            head = 1u;
        } else {
            heap_push(&heap, canonicalNodes[bvh2Node].left, canonical_surface_area(canonicalNodes[bvh2Node].left));
            heap_push(&heap, canonicalNodes[bvh2Node].right, canonical_surface_area(canonicalNodes[bvh2Node].right));
        }

        while heap_size(&heap) > 0 && (heap_size(&heap) + i32(head)) < 8 {
            let nodeId = heap_pop(&heap);
            if is_canonical_leaf(nodeId) {
                children[head] = nodeId;
                head += 1u;
            } else {
                heap_push(&heap, canonicalNodes[nodeId].left, canonical_surface_area(canonicalNodes[nodeId].left));
                heap_push(&heap, canonicalNodes[nodeId].right, canonical_surface_area(canonicalNodes[nodeId].right));
            }
        }

        while heap_size(&heap) > 0 {
            children[head] = heap_pop(&heap);
            head += 1u;
        }

        var isLeaf = array<bool, 8>(false, false, false, false, false, false, false, false);
        var internalCount = 0u;
        var leafCount = 0u;
        for (var i = 0u; i < head; i += 1u) {
            isLeaf[i] = is_canonical_leaf(children[i]);
            if isLeaf[i] {
                leafCount += 1u;
            } else {
                internalCount += 1u;
            }
        }

        let parentMin = canonicalNodes[bvh2Node].minPoint;
        let parentMax = canonicalNodes[bvh2Node].maxPoint;
        let parentCenter = (parentMin + parentMax) * 0.5;
        var childCenters = array<vec3<f32>, 8>();
        for (var i = 0u; i < head; i += 1u) {
            childCenters[i] = (canonicalNodes[children[i]].minPoint + canonicalNodes[children[i]].maxPoint) * 0.5;
        }

        var costs = array<array<f32, 8>, 8>();
        var assignments = array<u32, 8>(INVALID_INDEX, INVALID_INDEX, INVALID_INDEX, INVALID_INDEX, INVALID_INDEX, INVALID_INDEX, INVALID_INDEX, INVALID_INDEX);
        var slotEmpty = array<bool, 8>(true, true, true, true, true, true, true, true);
        for (var childIndex = 0u; childIndex < head; childIndex += 1u) {
            for (var slot = 0u; slot < 8u; slot += 1u) {
                let direction = vec3<f32>(
                    select(1.0, -1.0, (slot & 4u) != 0u),
                    select(1.0, -1.0, (slot & 2u) != 0u),
                    select(1.0, -1.0, (slot & 1u) != 0u));
                costs[slot][childIndex] = dot(childCenters[childIndex] - parentCenter, direction);
            }
        }

        loop {
            var minCost = 3.40282e+38;
            var minSlot = INVALID_INDEX;
            var minIndex = INVALID_INDEX;
            for (var childIndex = 0u; childIndex < head; childIndex += 1u) {
                if assignments[childIndex] == INVALID_INDEX {
                    for (var slot = 0u; slot < 8u; slot += 1u) {
                        if slotEmpty[slot] && costs[slot][childIndex] < minCost {
                            minCost = costs[slot][childIndex];
                            minSlot = slot;
                            minIndex = childIndex;
                        }
                    }
                }
            }
            if minSlot == INVALID_INDEX {
                break;
            }
            slotEmpty[minSlot] = false;
            assignments[minIndex] = minSlot;
        }

        var orderedChildren = array<u32, 8>(INVALID_INDEX, INVALID_INDEX, INVALID_INDEX, INVALID_INDEX, INVALID_INDEX, INVALID_INDEX, INVALID_INDEX, INVALID_INDEX);
        var orderedLeaf = array<bool, 8>(false, false, false, false, false, false, false, false);
        for (var i = 0u; i < head; i += 1u) {
            orderedChildren[assignments[i]] = children[i];
            orderedLeaf[assignments[i]] = isLeaf[i];
        }
        children = orderedChildren;
        isLeaf = orderedLeaf;

        let sxExp = encode_signed_exponent8(parentMax.x - parentMin.x);
        let syExp = encode_signed_exponent8(parentMax.y - parentMin.y);
        let szExp = encode_signed_exponent8(parentMax.z - parentMin.z);
        let scale = vec3<f32>(
            decode_exponent_scale(sxExp),
            decode_exponent_scale(syExp),
            decode_exponent_scale(szExp));

        var node = CWBVHNodeGPU();
        var nodeRef = make_invalid_node_ref();
        nodeRef.canonicalNodeIndex = bvh2Node;

        let childNodeBaseIndex = atomicAdd(&buildSignals[3u], internalCount);
        let primitiveBaseIndex = atomicAdd(&buildSignals[4u], leafCount);
        var imask = 0u;

        if internalCount + leafCount > 0u {
            let workerIncrement = select(0u, internalCount - 1u, internalCount > 0u);
            let baseWorkerOffset = atomicAdd(&buildSignals[2u], workerIncrement);
            var nodeOffset = 0u;
            var leafOffset = 0u;

            for (var slot = 0u; slot < 8u; slot += 1u) {
                if children[slot] == INVALID_INDEX {
                    continue;
                }

                var workerAddr = threadId;
                if nodeOffset > 0u {
                    workerAddr = baseWorkerOffset + nodeOffset;
                }

                nodeRef.childCanonicalIndices[slot] = children[slot];
                let childNode = canonicalNodes[children[slot]];
                let qlo = vec3<u32>(
                    quantize_lower_bound(childNode.minPoint.x, parentMin.x, scale.x),
                    quantize_lower_bound(childNode.minPoint.y, parentMin.y, scale.y),
                    quantize_lower_bound(childNode.minPoint.z, parentMin.z, scale.z));
                let qhi = vec3<u32>(
                    quantize_upper_bound(childNode.maxPoint.x, parentMin.x, scale.x),
                    quantize_upper_bound(childNode.maxPoint.y, parentMin.y, scale.y),
                    quantize_upper_bound(childNode.maxPoint.z, parentMin.z, scale.z));
                let arrayIndex = indexTable[slot];
                let bitOffset = offsetTable[slot];

                node.qlox[arrayIndex] |= qlo.x << bitOffset;
                node.qloy[arrayIndex] |= qlo.y << bitOffset;
                node.qloz[arrayIndex] |= qlo.z << bitOffset;
                node.qhix[arrayIndex] |= qhi.x << bitOffset;
                node.qhiy[arrayIndex] |= qhi.y << bitOffset;
                node.qhiz[arrayIndex] |= qhi.z << bitOffset;

                if !isLeaf[slot] {
                    imask |= 1u << slot;
                    node.info[arrayIndex] |= ((1u << 5u) | ((slot + 24u) & 0x1fu)) << bitOffset;
                    atomicStore(&buildFlags[workerAddr * 2u + 0u], children[slot]);
                    atomicStore(&buildFlags[workerAddr * 2u + 1u], childNodeBaseIndex + nodeOffset);
                    nodeOffset += 1u;
                } else {
                    leafInstanceIndices[primitiveBaseIndex + leafOffset] = childNode.instanceIndex;
                    node.info[arrayIndex] |= ((1u << 5u) | (leafOffset & 0x1fu)) << bitOffset;
                    leafOffset += 1u;
                }
            }
        }

        node.orxy = vec2<u32>(bitcast<u32>(parentMin.x), bitcast<u32>(parentMin.y));
        node.oeim = vec2<u32>(bitcast<u32>(parentMin.z), pack4xU8(sxExp, syExp, szExp, imask));
        node.ntbi = vec2<u32>(childNodeBaseIndex, primitiveBaseIndex);
        cwbvhNodes[bvh8Node] = node;
        cwbvhNodeRefs[bvh8Node] = nodeRef;
        atomicAdd(&buildSignals[5u], 1u);

        if internalCount == 0u {
            atomicStore(&buildDebug[threadId * 8u + 7u], 2u);
            return;
        }
    }
}

@compute @workgroup_size(1, 1, 1)
fn refit_canonical(@builtin(global_invocation_id) globalId: vec3<u32>) {
    if globalId.x != 0u || counters.refitRequested == 0u || uniforms.canonicalNodeCount == 0u {
        return;
    }

    for (var reverseIndex = 0u; reverseIndex < uniforms.canonicalNodeCount; reverseIndex += 1u) {
        let nodeIndex = uniforms.canonicalNodeCount - 1u - reverseIndex;
        var node = canonicalNodes[nodeIndex];

        if (node.flags & NODE_FLAG_LEAF) != 0u {
            let instanceIndex = node.instanceIndex;
            if instanceIndex < uniforms.instanceCount {
                let aabb = instanceAabbs[instanceIndex];
                node.minPoint = aabb.minPoint;
                node.maxPoint = aabb.maxPoint;
                node.subtreeLeafCount = 1u;
            }
            canonicalNodes[nodeIndex] = node;
            continue;
        }

        var hasBounds = false;
        var minPoint = vec3<f32>(0.0);
        var maxPoint = vec3<f32>(0.0);
        var subtreeLeafCount = 0u;

        if node.left != INVALID_INDEX && node.left < uniforms.canonicalNodeCount {
            let leftNode = canonicalNodes[node.left];
            minPoint = leftNode.minPoint;
            maxPoint = leftNode.maxPoint;
            subtreeLeafCount += leftNode.subtreeLeafCount;
            hasBounds = true;
        }

        if node.right != INVALID_INDEX && node.right < uniforms.canonicalNodeCount {
            let rightNode = canonicalNodes[node.right];
            minPoint = select(rightNode.minPoint, min(minPoint, rightNode.minPoint), hasBounds);
            maxPoint = select(rightNode.maxPoint, max(maxPoint, rightNode.maxPoint), hasBounds);
            subtreeLeafCount += rightNode.subtreeLeafCount;
            hasBounds = true;
        }

        if hasBounds {
            node.minPoint = minPoint;
            node.maxPoint = maxPoint;
            node.subtreeLeafCount = subtreeLeafCount;
            canonicalNodes[nodeIndex] = node;
        }
    }
}

@compute @workgroup_size(64, 1, 1)
fn refit_cwbvh(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let packedNodeIndex = globalId.x;
    if counters.refitRequested == 0u || packedNodeIndex >= uniforms.cwbvhNodeCount {
        return;
    }

    let canonicalNodeIndex = cwbvhNodeRefs[packedNodeIndex].canonicalNodeIndex;
    if canonicalNodeIndex == INVALID_INDEX || canonicalNodeIndex >= uniforms.canonicalNodeCount {
        return;
    }

    let canonicalNode = canonicalNodes[canonicalNodeIndex];
    let parentMin = canonicalNode.minPoint;
    let parentMax = canonicalNode.maxPoint;
    let extent = parentMax - parentMin;

    let sxExp = encode_signed_exponent8(extent.x);
    let syExp = encode_signed_exponent8(extent.y);
    let szExp = encode_signed_exponent8(extent.z);
    let scale = vec3<f32>(
        decode_exponent_scale(sxExp),
        decode_exponent_scale(syExp),
        decode_exponent_scale(szExp));

    var node = cwbvhNodes[packedNodeIndex];
    let imask = (node.oeim.y >> 24u) & 0xffu;

    node.orxy = vec2<u32>(bitcast<u32>(parentMin.x), bitcast<u32>(parentMin.y));
    node.oeim = vec2<u32>(bitcast<u32>(parentMin.z), pack4xU8(sxExp, syExp, szExp, imask));
    node.qlox = vec2<u32>(0u);
    node.qloy = vec2<u32>(0u);
    node.qloz = vec2<u32>(0u);
    node.qhix = vec2<u32>(0u);
    node.qhiy = vec2<u32>(0u);
    node.qhiz = vec2<u32>(0u);

    for (var childSlot = 0u; childSlot < 8u; childSlot += 1u) {
        let childCanonicalIndex = cwbvhNodeRefs[packedNodeIndex].childCanonicalIndices[childSlot];
        if childCanonicalIndex == INVALID_INDEX || childCanonicalIndex >= uniforms.canonicalNodeCount {
            continue;
        }

        let childNode = canonicalNodes[childCanonicalIndex];
        let bitOffset = (childSlot & 3u) * 8u;
        let arrayIndex = childSlot >> 2u;
        let qlo = vec3<u32>(
            quantize_lower_bound(childNode.minPoint.x, parentMin.x, scale.x),
            quantize_lower_bound(childNode.minPoint.y, parentMin.y, scale.y),
            quantize_lower_bound(childNode.minPoint.z, parentMin.z, scale.z));
        let qhi = vec3<u32>(
            quantize_upper_bound(childNode.maxPoint.x, parentMin.x, scale.x),
            quantize_upper_bound(childNode.maxPoint.y, parentMin.y, scale.y),
            quantize_upper_bound(childNode.maxPoint.z, parentMin.z, scale.z));

        if arrayIndex == 0u {
            node.qlox.x |= qlo.x << bitOffset;
            node.qloy.x |= qlo.y << bitOffset;
            node.qloz.x |= qlo.z << bitOffset;
            node.qhix.x |= qhi.x << bitOffset;
            node.qhiy.x |= qhi.y << bitOffset;
            node.qhiz.x |= qhi.z << bitOffset;
        } else {
            node.qlox.y |= qlo.x << bitOffset;
            node.qloy.y |= qlo.y << bitOffset;
            node.qloz.y |= qlo.z << bitOffset;
            node.qhix.y |= qhi.x << bitOffset;
            node.qhiy.y |= qhi.y << bitOffset;
            node.qhiz.y |= qhi.z << bitOffset;
        }
    }

    cwbvhNodes[packedNodeIndex] = node;
}
