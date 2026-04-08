// ============================================================================
// Virtual Geometry — Shared Data Structures (virtualgeometrydata.wgsl)
// ============================================================================

const HIERARCHY_QUEUE_SIZE_INDEX    : u32 = 0u;
const CLUSTER_QUEUE_SIZE_INDEX      : u32 = 1u;
const HW_VISIBLE_CLUSTER_COUNT_INDEX: u32 = 2u;
const READ_QUEUE_SIZE_INDEX         : u32 = 3u;
const SW_VISIBLE_CLUSTER_COUNT_INDEX: u32 = 4u;

// Maximum triangles per meshlet — must match Config::ClusterSize in VirtualGeometryBuilder.cpp.
// Used by the DRAW_INDIRECT_COUNT_DISABLED flat-vertex-stream variant.
const CLUSTER_SIZE: u32 = 128u;
const PAGE_NOT_INSTALLED_BIT: u32 = 0x80000000u;
const PAGE_INDEX_MASK       : u32 = 0x7FFFFFFFu;
const HIERARCHY_FORCE_TRAVERSAL_FLAG : u32 = 1u << 2u;
const HIERARCHY_STREAMING_MASK_SHIFT: u32 = 8u;
const HIERARCHY_ENABLED_MASK_SHIFT  : u32 = 16u;
const HIERARCHY_CHILD_MASK          : u32 = 0xFFu;
const CULLING_FLAG_FRUSTUM          : u32 = 1u << 0u;
const CULLING_FLAG_OCCLUSION        : u32 = 1u << 1u;
const CULLING_FLAG_STREAMING_PRIOS  : u32 = 1u << 2u;

// ============================================================================
// Meshlet descriptor table constants
//
// Page binary layout (word offsets from page base):
//   [0]  num_meshlets
//   [1]  position_data_size   (words)
//   [2]  normal_data_size     (words)
//   [3]  uv_data_size         (words)
//   [4]  index_data_size      (words, uint32-padded)
//   [5]  bone_weight_data_size(words)
//   [6]  dependency_count
//   [7]  group_count
//   [8 .. 8 + MAX_GROUPS_PER_PAGE - 1] local group -> first meshlet index
//   [PAGE_HEADER_WORDS .. PAGE_HEADER_WORDS + num_meshlets*MESHLET_DESC_WORDS - 1] descriptor table
//   then: position block | normal block | uv block | index block |
//         bone-weight block | dependency list
//
// Per-meshlet descriptor (MESHLET_DESC_WORDS = 34 words):
//   [0]  position_offset        word offset within position block
//   [1]  position_word_count
//   [2]  normal_offset          element index within normal block
//   [3]  normal_count           (= vertex_count)
//   [4]  uv_offset              float-pair index (= normal_offset * 2)
//   [5]  uv_count               (= vertex_count * 2)
//   [6]  index_offset           word offset within index block
//   [7]  index_word_count
//   [8]  vertex_count
//   [9]  triangle_count
//   [10] quantized_position_span_x
//   [11] quantized_position_span_y
//   [12] quantized_position_span_z
//   [13] quantization_factor
//   [14] min_position_x         bitcast<f32> of int32 quantised minimum
//   [15] min_position_y
//   [16] min_position_z
//   --- self LOD bounds ---
//   [17] self.center.x          (f32)
//   [18] self.center.y
//   [19] self.center.z
//   [20] self.radius            (f32)
//   [21] self.error             (f32)
//   --- parent LOD bounds ---
//   [22] parent.center.x        (f32)
//   [23] parent.center.y
//   [24] parent.center.z
//   [25] parent.radius          (f32)
//   [26] parent.error           (f32)
//   --- cluster cone ---
//   [27] cone.axis.x            (f32)
//   [28] cone.axis.y            (f32)
//   [29] cone.axis.z            (f32)
//   [30] cone.cutoff            (f32)
//   --- bone weights ---
//   [31] bone_weight_offset     word offset into bone-weight block
//   [32] bone_weights_per_vertex (u32; 0 = no skinning)
//   [33] packed_group_cluster   bits [5:0] = local group, bits [8:6] = cluster-in-group
// ============================================================================

const MAX_GROUPS_PER_PAGE : u32 = 64u;
const PAGE_GROUP_COUNT_WORD : u32 = 7u;
const PAGE_GROUP_TABLE_WORD : u32 = 8u;
const PAGE_HEADER_WORDS   : u32 = PAGE_GROUP_TABLE_WORD + MAX_GROUPS_PER_PAGE;
const MESHLET_DESC_WORDS  : u32 = 34u;

// Word offsets within a single meshlet descriptor
const DESC_POS_OFF    : u32 =  0u;
const DESC_POS_WORDS  : u32 =  1u;
const DESC_NORM_OFF   : u32 =  2u;
const DESC_NORM_COUNT : u32 =  3u;
const DESC_UV_OFF     : u32 =  4u;
const DESC_UV_COUNT   : u32 =  5u;
const DESC_IDX_OFF    : u32 =  6u;
const DESC_IDX_WORDS  : u32 =  7u;
const DESC_VERT_COUNT : u32 =  8u;
const DESC_TRI_COUNT  : u32 =  9u;
const DESC_POS_SPAN_X : u32 = 10u;
const DESC_POS_SPAN_Y : u32 = 11u;
const DESC_POS_SPAN_Z : u32 = 12u;
const DESC_QFACTOR    : u32 = 13u;
const DESC_MIN_X      : u32 = 14u;
const DESC_MIN_Y      : u32 = 15u;
const DESC_MIN_Z      : u32 = 16u;
const DESC_SELF_CX    : u32 = 17u;
const DESC_SELF_CY    : u32 = 18u;
const DESC_SELF_CZ    : u32 = 19u;
const DESC_SELF_R     : u32 = 20u;
const DESC_SELF_ERR   : u32 = 21u;
const DESC_PAR_CX     : u32 = 22u;
const DESC_PAR_CY     : u32 = 23u;
const DESC_PAR_CZ     : u32 = 24u;
const DESC_PAR_R      : u32 = 25u;
const DESC_PAR_ERR    : u32 = 26u;
const DESC_CONE_AXIS_X: u32 = 27u;
const DESC_CONE_AXIS_Y: u32 = 28u;
const DESC_CONE_AXIS_Z: u32 = 29u;
const DESC_CONE_CUTOFF: u32 = 30u;
const DESC_BW_OFF     : u32 = 31u;
const DESC_BW_PER_V   : u32 = 32u;
const DESC_GROUP_CLUSTER: u32 = 33u;

// ============================================================================
// Structs
// ============================================================================

struct AABB {
    min: vec3<f32>,
    max: vec3<f32>,
}

struct LODBounds {
    center: vec3<f32>,
    radius: f32,
    error : f32,
}

struct ClusterCone {
    axis  : vec3<f32>,
    cutoff: f32,
}

struct CullingUniforms {
    view            : mat4x4<f32>,
    proj            : mat4x4<f32>,
    viewPosition    : vec4<f32>,
    viewport        : vec2<u32>,
    error           : f32,
    instances_count : u32,
    clusters_count  : u32,
    nearPlane       : f32,
    farPlane        : f32,
    hiZLevels       : u32,
    cullingFlags    : u32,
}


struct InstanceData {
    modelMatrix          : mat4x4<f32>,
    hierarchyStartOffset : u32,
    quantization_factor  : u32,
    unit_scale_bits      : u32,
    pageTableOffset      : u32,
    materialIndex        : u32,
    meshPartTransformsOffset : u32,
    _padding0            : u32,
    _padding1            : u32,
}

struct VirtualMeshHierarchy {
    max_x               : f32,
    max_y               : f32,
    max_z               : f32,
    min_x               : f32,
    min_y               : f32,
    min_z               : f32,
    max_center_x        : f32,
    max_center_y        : f32,
    max_center_z        : f32,
    max_radius          : f32,
    min_lod_error       : f32,
    max_parent_lod_error: f32,
    child_start         : u32,
    child_count         : u32,
    pageIndex           : u32,
    meshPartIndex       : u32,
    flags               : u32,
}

struct QueueElement {
    index         : u32,
    instanceIndex : u32,
    pageIndex     : u32,
    _padding      : u32,
}

struct IndirectDispatchArgs {
    x: u32,
    y: u32,
    z: u32,
}

struct PageTableEntry {
    bufferOffset : u32,
    size         : u32,
    clusterOffset: u32,
    clusterCount : u32,
    isInstalled  : u32,
    prioritySlot : u32,
    _pad1        : u32,
    _pad2        : u32,
}

struct VisibleClusterInfo {
    pageIndex             : u32,
    pageLocalClusterIndex : u32,
    instanceIndex         : u32,
    _padding              : u32,
}

// ============================================================================
// Shared page-reading helpers
// ============================================================================

/// @brief Returns the word base offset of a page's data in the global pages buffer given its PageTableEntry.
/// @param entry The page table entry whose buffer offset is used.
/// @returns The word (u32) index at which the page's data begins.
fn pageWordBase(entry: PageTableEntry) -> u32 {
    return entry.bufferOffset / 4u;
}

/// @brief Returns the absolute word offset of a specific field within a meshlet descriptor, relative to the start of the pages buffer.
/// @param pageBase The word base of the page containing this meshlet.
/// @param numMeshlets The number of meshlets in the page (unused, kept for API symmetry).
/// @param localIdx The page-local meshlet index.
/// @param fieldOff The word offset of the desired field within a single meshlet descriptor.
/// @returns The absolute word index into the pages buffer for the requested descriptor field.
fn descWordOffset(pageBase: u32, numMeshlets: u32, localIdx: u32, fieldOff: u32) -> u32 {
    return pageBase + PAGE_HEADER_WORDS + localIdx * MESHLET_DESC_WORDS + fieldOff;
}

/// @brief Returns the word offset at which vertex data blocks begin within a page, immediately after the descriptor table.
/// @param pageBase The word base of the page in the pages buffer.
/// @param numMeshlets The number of meshlets in the page, used to skip past the full descriptor table.
/// @returns The absolute word index into the pages buffer for the start of the position block.
fn vertexDataBase(pageBase: u32, numMeshlets: u32) -> u32 {
    return pageBase + PAGE_HEADER_WORDS + numMeshlets * MESHLET_DESC_WORDS;
}

struct MeshletDescriptor {
    position_offset    : u32,
    position_count     : u32,
    normal_offset      : u32,
    normal_count       : u32,
    uv_offset          : u32,
    uv_count           : u32,
    index_offset       : u32,
    index_count        : u32,
    vertex_count       : u32,
    triangle_count     : u32,
    quantized_position_span_x : u32,
    quantized_position_span_y : u32,
    quantized_position_span_z : u32,
    quantization_factor: u32,
    min_position_x     : u32,  // bitcast to f32
    min_position_y     : u32,
    min_position_z     : u32,
    // self LOD bounds
    self_center_x      : u32,  // bitcast to f32
    self_center_y      : u32,
    self_center_z      : u32,
    self_radius        : u32,
    self_error         : u32,
    // parent LOD bounds
    parent_center_x    : u32,
    parent_center_y    : u32,
    parent_center_z    : u32,
    parent_radius      : u32,
    parent_error       : u32,
    // cluster cone
    cone_axis_x        : u32,
    cone_axis_y        : u32,
    cone_axis_z        : u32,
    cone_cutoff        : u32,
    // bone weights
    bone_weight_offset    : u32,
    bone_weights_per_vertex: u32,
    packed_group_cluster  : u32,
}

/// @brief Computes the number of bits needed to represent a quantized position span value.
/// @param span The quantized span value along one axis.
/// @returns The minimum number of bits required to store values in the range [0, span], or 0 if span is zero.
fn quantizedSpanToBitCount(span: u32) -> u32 {
    if (span == 0u) {
        return 0u;
    }
    return 32u - countLeadingZeros(span);
}

struct PageHeader {
    num_meshlets          : u32,
    position_data_size    : u32,
    normal_data_size      : u32,
    uv_data_size          : u32,
    index_data_size       : u32,
    bone_weight_data_size : u32,
    dependency_count      : u32,
    group_count           : u32,
}
