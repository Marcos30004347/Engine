const WORKGROUP_SIZE_X = 8u;
const WORKGROUP_SIZE_Y = 8u;
const INVALID_INDEX = 0xffffffffu;
const DEBUG_MODE_HEATMAP = 1u;
const DEBUG_MODE_RBVH_CARD_AABB = 16u;
const DEBUG_MODE_BLAS_CANDIDATE_COUNT = 18u;
const DEBUG_MODE_BLAS_SORTED_CANDIDATE_DISTANCE = 19u;
const DEBUG_MODE_BLAS_TESTS_PER_RAY = 20u;
const DEBUG_MODE_TLAS_CANDIDATE_OVERFLOW = 21u;
const DEBUG_MODE_PRIMARY_RAY_HEATMAP = 22u;
const DEBUG_HIT_INSTANCE_RAY_GENERATED = 0xfffffffbu;
const DEBUG_HIT_INSTANCE_INVALID_RAY = 0xfffffffcu;
const DEBUG_HIT_INSTANCE_HEATMAP = 0xfffffffdu;
const DEBUG_HIT_INSTANCE_MARKER = 0xfffffffeu;
const DEBUG_HIT_TRACE_DISABLED = 0xfffffffeu;
const DEBUG_HIT_RBVH_CARD_AABB = 0xfffffffau;
const TLAS_NODE_DEPTH_SHIFT = 8u;
const OVERLAY_FLAG_TLAS_BOUNDS = 1u;
const OVERLAY_FLAG_TRACE_ENABLED = 2u;
const OVERLAY_FLAG_TLAS_CWBVH_BOUNDS = 4u;
const MAX_STACK_DEPTH = 16u;
const TLAS_TRAVERSAL_STACK_SIZE = MAX_STACK_DEPTH;
const TLAS_BLAS_CANDIDATE_STACK_SIZE = 32u;
const BLAS_CANDIDATE_COUNT_OVERFLOW_BIT = 0x80000000u;
const TLAS_GROUP_SLOT_ORDER_MASK_FLAG = 0x00800000u;
const TLAS_DOBB_NODE_FLAG_ORIENTED = 1u;
const TLAS_DOBB_ROTATION_AXIS_COUNT = 13u;
const TLAS_DOBB_ROTATION_ANGLE_COUNT = 8u;
const TLAS_DOBB_ROTATION_COUNT = TLAS_DOBB_ROTATION_AXIS_COUNT * TLAS_DOBB_ROTATION_ANGLE_COUNT;
const TLAS_DOBB_ROTATION_TABLE = array<mat3x3<f32>, 104>(
    mat3x3<f32>(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.0, 0.0, 1.0)),
    mat3x3<f32>(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 0.9238795325, 0.3826834324), vec3<f32>(0.0, -0.3826834324, 0.9238795325)),
    mat3x3<f32>(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 0.7071067812, 0.7071067812), vec3<f32>(0.0, -0.7071067812, 0.7071067812)),
    mat3x3<f32>(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 0.3826834324, 0.9238795325), vec3<f32>(0.0, -0.9238795325, 0.3826834324)),
    mat3x3<f32>(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 0.0, 1.0), vec3<f32>(0.0, -1.0, 0.0)),
    mat3x3<f32>(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, -0.3826834324, 0.9238795325), vec3<f32>(0.0, -0.9238795325, -0.3826834324)),
    mat3x3<f32>(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, -0.7071067812, 0.7071067812), vec3<f32>(0.0, -0.7071067812, -0.7071067812)),
    mat3x3<f32>(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, -0.9238795325, 0.3826834324), vec3<f32>(0.0, -0.3826834324, -0.9238795325)),
    mat3x3<f32>(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.0, 0.0, 1.0)),
    mat3x3<f32>(vec3<f32>(0.9238795325, 0.0, -0.3826834324), vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.3826834324, 0.0, 0.9238795325)),
    mat3x3<f32>(vec3<f32>(0.7071067812, 0.0, -0.7071067812), vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.7071067812, 0.0, 0.7071067812)),
    mat3x3<f32>(vec3<f32>(0.3826834324, 0.0, -0.9238795325), vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.9238795325, 0.0, 0.3826834324)),
    mat3x3<f32>(vec3<f32>(0.0, 0.0, -1.0), vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(1.0, 0.0, 0.0)),
    mat3x3<f32>(vec3<f32>(-0.3826834324, 0.0, -0.9238795325), vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.9238795325, 0.0, -0.3826834324)),
    mat3x3<f32>(vec3<f32>(-0.7071067812, 0.0, -0.7071067812), vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.7071067812, 0.0, -0.7071067812)),
    mat3x3<f32>(vec3<f32>(-0.9238795325, 0.0, -0.3826834324), vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.3826834324, 0.0, -0.9238795325)),
    mat3x3<f32>(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.0, 0.0, 1.0)),
    mat3x3<f32>(vec3<f32>(0.9238795325, 0.3826834324, 0.0), vec3<f32>(-0.3826834324, 0.9238795325, 0.0), vec3<f32>(0.0, 0.0, 1.0)),
    mat3x3<f32>(vec3<f32>(0.7071067812, 0.7071067812, 0.0), vec3<f32>(-0.7071067812, 0.7071067812, 0.0), vec3<f32>(0.0, 0.0, 1.0)),
    mat3x3<f32>(vec3<f32>(0.3826834324, 0.9238795325, 0.0), vec3<f32>(-0.9238795325, 0.3826834324, 0.0), vec3<f32>(0.0, 0.0, 1.0)),
    mat3x3<f32>(vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(-1.0, 0.0, 0.0), vec3<f32>(0.0, 0.0, 1.0)),
    mat3x3<f32>(vec3<f32>(-0.3826834324, 0.9238795325, 0.0), vec3<f32>(-0.9238795325, -0.3826834324, 0.0), vec3<f32>(0.0, 0.0, 1.0)),
    mat3x3<f32>(vec3<f32>(-0.7071067812, 0.7071067812, 0.0), vec3<f32>(-0.7071067812, -0.7071067812, 0.0), vec3<f32>(0.0, 0.0, 1.0)),
    mat3x3<f32>(vec3<f32>(-0.9238795325, 0.3826834324, 0.0), vec3<f32>(-0.3826834324, -0.9238795325, 0.0), vec3<f32>(0.0, 0.0, 1.0)),
    mat3x3<f32>(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.0, 0.0, 1.0)),
    mat3x3<f32>(vec3<f32>(0.9619397663, 0.03806023374, -0.2705980501), vec3<f32>(0.03806023374, 0.9619397663, 0.2705980501), vec3<f32>(0.2705980501, -0.2705980501, 0.9238795325)),
    mat3x3<f32>(vec3<f32>(0.8535533906, 0.1464466094, -0.5), vec3<f32>(0.1464466094, 0.8535533906, 0.5), vec3<f32>(0.5, -0.5, 0.7071067812)),
    mat3x3<f32>(vec3<f32>(0.6913417162, 0.3086582838, -0.6532814824), vec3<f32>(0.3086582838, 0.6913417162, 0.6532814824), vec3<f32>(0.6532814824, -0.6532814824, 0.3826834324)),
    mat3x3<f32>(vec3<f32>(0.5, 0.5, -0.7071067812), vec3<f32>(0.5, 0.5, 0.7071067812), vec3<f32>(0.7071067812, -0.7071067812, 0.0)),
    mat3x3<f32>(vec3<f32>(0.3086582838, 0.6913417162, -0.6532814824), vec3<f32>(0.6913417162, 0.3086582838, 0.6532814824), vec3<f32>(0.6532814824, -0.6532814824, -0.3826834324)),
    mat3x3<f32>(vec3<f32>(0.1464466094, 0.8535533906, -0.5), vec3<f32>(0.8535533906, 0.1464466094, 0.5), vec3<f32>(0.5, -0.5, -0.7071067812)),
    mat3x3<f32>(vec3<f32>(0.03806023374, 0.9619397663, -0.2705980501), vec3<f32>(0.9619397663, 0.03806023374, 0.2705980501), vec3<f32>(0.2705980501, -0.2705980501, -0.9238795325)),
    mat3x3<f32>(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.0, 0.0, 1.0)),
    mat3x3<f32>(vec3<f32>(0.9619397663, -0.03806023374, 0.2705980501), vec3<f32>(-0.03806023374, 0.9619397663, 0.2705980501), vec3<f32>(-0.2705980501, -0.2705980501, 0.9238795325)),
    mat3x3<f32>(vec3<f32>(0.8535533906, -0.1464466094, 0.5), vec3<f32>(-0.1464466094, 0.8535533906, 0.5), vec3<f32>(-0.5, -0.5, 0.7071067812)),
    mat3x3<f32>(vec3<f32>(0.6913417162, -0.3086582838, 0.6532814824), vec3<f32>(-0.3086582838, 0.6913417162, 0.6532814824), vec3<f32>(-0.6532814824, -0.6532814824, 0.3826834324)),
    mat3x3<f32>(vec3<f32>(0.5, -0.5, 0.7071067812), vec3<f32>(-0.5, 0.5, 0.7071067812), vec3<f32>(-0.7071067812, -0.7071067812, 0.0)),
    mat3x3<f32>(vec3<f32>(0.3086582838, -0.6913417162, 0.6532814824), vec3<f32>(-0.6913417162, 0.3086582838, 0.6532814824), vec3<f32>(-0.6532814824, -0.6532814824, -0.3826834324)),
    mat3x3<f32>(vec3<f32>(0.1464466094, -0.8535533906, 0.5), vec3<f32>(-0.8535533906, 0.1464466094, 0.5), vec3<f32>(-0.5, -0.5, -0.7071067812)),
    mat3x3<f32>(vec3<f32>(0.03806023374, -0.9619397663, 0.2705980501), vec3<f32>(-0.9619397663, 0.03806023374, 0.2705980501), vec3<f32>(-0.2705980501, -0.2705980501, -0.9238795325)),
    mat3x3<f32>(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.0, 0.0, 1.0)),
    mat3x3<f32>(vec3<f32>(0.9619397663, 0.2705980501, 0.03806023374), vec3<f32>(-0.2705980501, 0.9238795325, 0.2705980501), vec3<f32>(0.03806023374, -0.2705980501, 0.9619397663)),
    mat3x3<f32>(vec3<f32>(0.8535533906, 0.5, 0.1464466094), vec3<f32>(-0.5, 0.7071067812, 0.5), vec3<f32>(0.1464466094, -0.5, 0.8535533906)),
    mat3x3<f32>(vec3<f32>(0.6913417162, 0.6532814824, 0.3086582838), vec3<f32>(-0.6532814824, 0.3826834324, 0.6532814824), vec3<f32>(0.3086582838, -0.6532814824, 0.6913417162)),
    mat3x3<f32>(vec3<f32>(0.5, 0.7071067812, 0.5), vec3<f32>(-0.7071067812, 0.0, 0.7071067812), vec3<f32>(0.5, -0.7071067812, 0.5)),
    mat3x3<f32>(vec3<f32>(0.3086582838, 0.6532814824, 0.6913417162), vec3<f32>(-0.6532814824, -0.3826834324, 0.6532814824), vec3<f32>(0.6913417162, -0.6532814824, 0.3086582838)),
    mat3x3<f32>(vec3<f32>(0.1464466094, 0.5, 0.8535533906), vec3<f32>(-0.5, -0.7071067812, 0.5), vec3<f32>(0.8535533906, -0.5, 0.1464466094)),
    mat3x3<f32>(vec3<f32>(0.03806023374, 0.2705980501, 0.9619397663), vec3<f32>(-0.2705980501, -0.9238795325, 0.2705980501), vec3<f32>(0.9619397663, -0.2705980501, 0.03806023374)),
    mat3x3<f32>(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.0, 0.0, 1.0)),
    mat3x3<f32>(vec3<f32>(0.9619397663, -0.2705980501, -0.03806023374), vec3<f32>(0.2705980501, 0.9238795325, 0.2705980501), vec3<f32>(-0.03806023374, -0.2705980501, 0.9619397663)),
    mat3x3<f32>(vec3<f32>(0.8535533906, -0.5, -0.1464466094), vec3<f32>(0.5, 0.7071067812, 0.5), vec3<f32>(-0.1464466094, -0.5, 0.8535533906)),
    mat3x3<f32>(vec3<f32>(0.6913417162, -0.6532814824, -0.3086582838), vec3<f32>(0.6532814824, 0.3826834324, 0.6532814824), vec3<f32>(-0.3086582838, -0.6532814824, 0.6913417162)),
    mat3x3<f32>(vec3<f32>(0.5, -0.7071067812, -0.5), vec3<f32>(0.7071067812, 0.0, 0.7071067812), vec3<f32>(-0.5, -0.7071067812, 0.5)),
    mat3x3<f32>(vec3<f32>(0.3086582838, -0.6532814824, -0.6913417162), vec3<f32>(0.6532814824, -0.3826834324, 0.6532814824), vec3<f32>(-0.6913417162, -0.6532814824, 0.3086582838)),
    mat3x3<f32>(vec3<f32>(0.1464466094, -0.5, -0.8535533906), vec3<f32>(0.5, -0.7071067812, 0.5), vec3<f32>(-0.8535533906, -0.5, 0.1464466094)),
    mat3x3<f32>(vec3<f32>(0.03806023374, -0.2705980501, -0.9619397663), vec3<f32>(0.2705980501, -0.9238795325, 0.2705980501), vec3<f32>(-0.9619397663, -0.2705980501, 0.03806023374)),
    mat3x3<f32>(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.0, 0.0, 1.0)),
    mat3x3<f32>(vec3<f32>(0.9238795325, 0.2705980501, -0.2705980501), vec3<f32>(-0.2705980501, 0.9619397663, 0.03806023374), vec3<f32>(0.2705980501, 0.03806023374, 0.9619397663)),
    mat3x3<f32>(vec3<f32>(0.7071067812, 0.5, -0.5), vec3<f32>(-0.5, 0.8535533906, 0.1464466094), vec3<f32>(0.5, 0.1464466094, 0.8535533906)),
    mat3x3<f32>(vec3<f32>(0.3826834324, 0.6532814824, -0.6532814824), vec3<f32>(-0.6532814824, 0.6913417162, 0.3086582838), vec3<f32>(0.6532814824, 0.3086582838, 0.6913417162)),
    mat3x3<f32>(vec3<f32>(0.0, 0.7071067812, -0.7071067812), vec3<f32>(-0.7071067812, 0.5, 0.5), vec3<f32>(0.7071067812, 0.5, 0.5)),
    mat3x3<f32>(vec3<f32>(-0.3826834324, 0.6532814824, -0.6532814824), vec3<f32>(-0.6532814824, 0.3086582838, 0.6913417162), vec3<f32>(0.6532814824, 0.6913417162, 0.3086582838)),
    mat3x3<f32>(vec3<f32>(-0.7071067812, 0.5, -0.5), vec3<f32>(-0.5, 0.1464466094, 0.8535533906), vec3<f32>(0.5, 0.8535533906, 0.1464466094)),
    mat3x3<f32>(vec3<f32>(-0.9238795325, 0.2705980501, -0.2705980501), vec3<f32>(-0.2705980501, 0.03806023374, 0.9619397663), vec3<f32>(0.2705980501, 0.9619397663, 0.03806023374)),
    mat3x3<f32>(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.0, 0.0, 1.0)),
    mat3x3<f32>(vec3<f32>(0.9238795325, -0.2705980501, -0.2705980501), vec3<f32>(0.2705980501, 0.9619397663, -0.03806023374), vec3<f32>(0.2705980501, -0.03806023374, 0.9619397663)),
    mat3x3<f32>(vec3<f32>(0.7071067812, -0.5, -0.5), vec3<f32>(0.5, 0.8535533906, -0.1464466094), vec3<f32>(0.5, -0.1464466094, 0.8535533906)),
    mat3x3<f32>(vec3<f32>(0.3826834324, -0.6532814824, -0.6532814824), vec3<f32>(0.6532814824, 0.6913417162, -0.3086582838), vec3<f32>(0.6532814824, -0.3086582838, 0.6913417162)),
    mat3x3<f32>(vec3<f32>(0.0, -0.7071067812, -0.7071067812), vec3<f32>(0.7071067812, 0.5, -0.5), vec3<f32>(0.7071067812, -0.5, 0.5)),
    mat3x3<f32>(vec3<f32>(-0.3826834324, -0.6532814824, -0.6532814824), vec3<f32>(0.6532814824, 0.3086582838, -0.6913417162), vec3<f32>(0.6532814824, -0.6913417162, 0.3086582838)),
    mat3x3<f32>(vec3<f32>(-0.7071067812, -0.5, -0.5), vec3<f32>(0.5, 0.1464466094, -0.8535533906), vec3<f32>(0.5, -0.8535533906, 0.1464466094)),
    mat3x3<f32>(vec3<f32>(-0.9238795325, -0.2705980501, -0.2705980501), vec3<f32>(0.2705980501, 0.03806023374, -0.9619397663), vec3<f32>(0.2705980501, -0.9619397663, 0.03806023374)),
    mat3x3<f32>(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.0, 0.0, 1.0)),
    mat3x3<f32>(vec3<f32>(0.9492530217, 0.2463158719, -0.1955688935), vec3<f32>(-0.1955688935, 0.9492530217, 0.2463158719), vec3<f32>(0.2463158719, -0.1955688935, 0.9492530217)),
    mat3x3<f32>(vec3<f32>(0.8047378541, 0.5058793634, -0.3106172175), vec3<f32>(-0.3106172175, 0.8047378541, 0.5058793634), vec3<f32>(0.5058793634, -0.3106172175, 0.8047378541)),
    mat3x3<f32>(vec3<f32>(0.5884556216, 0.739174286, -0.3276299076), vec3<f32>(-0.3276299076, 0.5884556216, 0.739174286), vec3<f32>(0.739174286, -0.3276299076, 0.5884556216)),
    mat3x3<f32>(vec3<f32>(0.3333333333, 0.9106836025, -0.2440169359), vec3<f32>(-0.2440169359, 0.3333333333, 0.9106836025), vec3<f32>(0.9106836025, -0.2440169359, 0.3333333333)),
    mat3x3<f32>(vec3<f32>(0.07821104509, 0.9942965742, -0.07250761934), vec3<f32>(-0.07250761934, 0.07821104509, 0.9942965742), vec3<f32>(0.9942965742, -0.07250761934, 0.07821104509)),
    mat3x3<f32>(vec3<f32>(-0.1380711875, 0.9772838842, 0.1607873033), vec3<f32>(0.1607873033, -0.1380711875, 0.9772838842), vec3<f32>(0.9772838842, 0.1607873033, -0.1380711875)),
    mat3x3<f32>(vec3<f32>(-0.282586355, 0.8622355602, 0.4203507948), vec3<f32>(0.4203507948, -0.282586355, 0.8622355602), vec3<f32>(0.8622355602, 0.4203507948, -0.282586355)),
    mat3x3<f32>(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.0, 0.0, 1.0)),
    mat3x3<f32>(vec3<f32>(0.9492530217, -0.1955688935, -0.2463158719), vec3<f32>(0.2463158719, 0.9492530217, 0.1955688935), vec3<f32>(0.1955688935, -0.2463158719, 0.9492530217)),
    mat3x3<f32>(vec3<f32>(0.8047378541, -0.3106172175, -0.5058793634), vec3<f32>(0.5058793634, 0.8047378541, 0.3106172175), vec3<f32>(0.3106172175, -0.5058793634, 0.8047378541)),
    mat3x3<f32>(vec3<f32>(0.5884556216, -0.3276299076, -0.739174286), vec3<f32>(0.739174286, 0.5884556216, 0.3276299076), vec3<f32>(0.3276299076, -0.739174286, 0.5884556216)),
    mat3x3<f32>(vec3<f32>(0.3333333333, -0.2440169359, -0.9106836025), vec3<f32>(0.9106836025, 0.3333333333, 0.2440169359), vec3<f32>(0.2440169359, -0.9106836025, 0.3333333333)),
    mat3x3<f32>(vec3<f32>(0.07821104509, -0.07250761934, -0.9942965742), vec3<f32>(0.9942965742, 0.07821104509, 0.07250761934), vec3<f32>(0.07250761934, -0.9942965742, 0.07821104509)),
    mat3x3<f32>(vec3<f32>(-0.1380711875, 0.1607873033, -0.9772838842), vec3<f32>(0.9772838842, -0.1380711875, -0.1607873033), vec3<f32>(-0.1607873033, -0.9772838842, -0.1380711875)),
    mat3x3<f32>(vec3<f32>(-0.282586355, 0.4203507948, -0.8622355602), vec3<f32>(0.8622355602, -0.282586355, -0.4203507948), vec3<f32>(-0.4203507948, -0.8622355602, -0.282586355)),
    mat3x3<f32>(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.0, 0.0, 1.0)),
    mat3x3<f32>(vec3<f32>(0.9492530217, 0.1955688935, 0.2463158719), vec3<f32>(-0.2463158719, 0.9492530217, 0.1955688935), vec3<f32>(-0.1955688935, -0.2463158719, 0.9492530217)),
    mat3x3<f32>(vec3<f32>(0.8047378541, 0.3106172175, 0.5058793634), vec3<f32>(-0.5058793634, 0.8047378541, 0.3106172175), vec3<f32>(-0.3106172175, -0.5058793634, 0.8047378541)),
    mat3x3<f32>(vec3<f32>(0.5884556216, 0.3276299076, 0.739174286), vec3<f32>(-0.739174286, 0.5884556216, 0.3276299076), vec3<f32>(-0.3276299076, -0.739174286, 0.5884556216)),
    mat3x3<f32>(vec3<f32>(0.3333333333, 0.2440169359, 0.9106836025), vec3<f32>(-0.9106836025, 0.3333333333, 0.2440169359), vec3<f32>(-0.2440169359, -0.9106836025, 0.3333333333)),
    mat3x3<f32>(vec3<f32>(0.07821104509, 0.07250761934, 0.9942965742), vec3<f32>(-0.9942965742, 0.07821104509, 0.07250761934), vec3<f32>(-0.07250761934, -0.9942965742, 0.07821104509)),
    mat3x3<f32>(vec3<f32>(-0.1380711875, -0.1607873033, 0.9772838842), vec3<f32>(-0.9772838842, -0.1380711875, -0.1607873033), vec3<f32>(0.1607873033, -0.9772838842, -0.1380711875)),
    mat3x3<f32>(vec3<f32>(-0.282586355, -0.4203507948, 0.8622355602), vec3<f32>(-0.8622355602, -0.282586355, -0.4203507948), vec3<f32>(0.4203507948, -0.8622355602, -0.282586355)),
    mat3x3<f32>(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.0, 0.0, 1.0)),
    mat3x3<f32>(vec3<f32>(0.9492530217, 0.1955688935, -0.2463158719), vec3<f32>(-0.2463158719, 0.9492530217, -0.1955688935), vec3<f32>(0.1955688935, 0.2463158719, 0.9492530217)),
    mat3x3<f32>(vec3<f32>(0.8047378541, 0.3106172175, -0.5058793634), vec3<f32>(-0.5058793634, 0.8047378541, -0.3106172175), vec3<f32>(0.3106172175, 0.5058793634, 0.8047378541)),
    mat3x3<f32>(vec3<f32>(0.5884556216, 0.3276299076, -0.739174286), vec3<f32>(-0.739174286, 0.5884556216, -0.3276299076), vec3<f32>(0.3276299076, 0.739174286, 0.5884556216)),
    mat3x3<f32>(vec3<f32>(0.3333333333, 0.2440169359, -0.9106836025), vec3<f32>(-0.9106836025, 0.3333333333, -0.2440169359), vec3<f32>(0.2440169359, 0.9106836025, 0.3333333333)),
    mat3x3<f32>(vec3<f32>(0.07821104509, 0.07250761934, -0.9942965742), vec3<f32>(-0.9942965742, 0.07821104509, -0.07250761934), vec3<f32>(0.07250761934, 0.9942965742, 0.07821104509)),
    mat3x3<f32>(vec3<f32>(-0.1380711875, -0.1607873033, -0.9772838842), vec3<f32>(-0.9772838842, -0.1380711875, 0.1607873033), vec3<f32>(-0.1607873033, 0.9772838842, -0.1380711875)),
    mat3x3<f32>(vec3<f32>(-0.282586355, -0.4203507948, -0.8622355602), vec3<f32>(-0.8622355602, -0.282586355, 0.4203507948), vec3<f32>(-0.4203507948, 0.8622355602, -0.282586355))
);
const RBVH_CARD_DDA_MAX_STEPS = 1024u;
const RBVH_CARD_DDA_MAX_MIP_LEVEL = 10u;
const RAY_SORT_BIN_COUNT = 256u;
const STREAM_TASK_KIND_TLAS_GROUP = 0u;
const STREAM_TASK_KIND_BLAS_GROUP = 1u;
const STREAM_PACKET_RAY_CAPACITY = 32u;
const STREAM_PACKETS_PER_WORKGROUP = 4u;
const STREAM_CHILD_PACKET_CAPACITY = 8u;
const STREAM_BUCKETS_PER_PACKET = 32u;
const STREAM_LOCAL_BUCKET_COUNT = STREAM_PACKETS_PER_WORKGROUP * STREAM_BUCKETS_PER_PACKET;
const STREAM_LOCAL_BUCKET_SENTINEL = STREAM_LOCAL_BUCKET_COUNT - 1u;
const STREAM_WORKGROUP_QUEUE_CAPACITY = 32u;
const STREAM_MAX_LOCAL_NODE_ITERATIONS = 16u;
const STREAM_GLOBAL_HASH_SHARD_COUNT = 8u;
const STREAM_GLOBAL_HASH_MAX_PROBES = 8u;
const STREAM_GLOBAL_HASH_SLOT_MULTIPLIER = 4u;
const STREAM_GLOBAL_HASH_BLOCK_MULTIPLIER = 2u;
const STREAM_GLOBAL_HASH_EMPTY = 0u;
const STREAM_GLOBAL_HASH_CLAIMING = 0xffffffffu;
const STREAM_FLATTEN_SCRATCH_REF_CAPACITY = 256u;
const STREAM_FLATTEN_MAX_PACKETS_PER_BATCH = STREAM_FLATTEN_SCRATCH_REF_CAPACITY / STREAM_PACKET_RAY_CAPACITY;
const STREAM_BUCKET_CLAIMING = 0xffffffffu;

#include "software-raytrace-ray.wgsl"

struct Uniforms {
    width: u32,
    height: u32,
    tlasNodeCount: u32,
    debugMode: u32,
    inverseViewProjection: mat4x4<f32>,
    cameraPosition: vec3<f32>,
    tanHalfFovY: f32,
    cameraForward: vec3<f32>,
    aspectRatio: f32,
    cameraRight: vec3<f32>,
    tMin: f32,
    cameraUp: vec3<f32>,
    tMax: f32,
    backgroundColor: vec4<f32>,
    heatmapMaxIterations: u32,
    rayCount: u32,
    overlayFlags: u32,
    rayDispatchWidth: u32,
    overlayColorAndThickness: vec4<f32>,
    traceControl: vec4<f32>,
    primitiveMode: u32,
    rbvhLodLevel: u32,
    rbvhDepthAtlasTextureWidth: u32,
    rbvhDepthAtlasTextureHeight: u32,
    traceFlags: vec4<u32>,
    twoStageControl: vec4<u32>,
};

struct InstanceGPU {
    transform: mat4x4<f32>,
    inverseTransform: mat4x4<f32>,
    previousTransform: mat4x4<f32>,
    aabbMin: vec3<f32>,
    blasHandle: u32,
    aabbMax: vec3<f32>,
    sceneInstanceId: u32,
    flags: u32,
    _padding: array<u32, 3>,
};

struct InstanceAABBGPU {
    minPoint: vec3<f32>,
    instanceIndex: u32,
    maxPoint: vec3<f32>,
    blasHandle: u32,
};

struct BVH4NodeGPU {
    childMinX: vec4<f32>,
    childMinY: vec4<f32>,
    childMinZ: vec4<f32>,
    childIndex: vec4<u32>,
    childMaxX: vec4<f32>,
    childMaxY: vec4<f32>,
    childMaxZ: vec4<f32>,
    childMeta: vec4<u32>,
};

struct WorldInstanceRangeGPU {
    instanceIndex: u32,
    sceneInstanceId: u32,
    blasHandle: u32,
    flags: u32,
    worldCWBVHNodeOffset: u32,
    worldCWBVHNodeCount: u32,
    worldBVH8NodeOffset: u32,
    worldBVH8NodeCount: u32,
    worldTriangleOffset: u32,
    worldTriangleCount: u32,
    worldLeafRemapOffset: u32,
    worldLeafRemapCount: u32,
    valid: u32,
    dynamic: u32,
    needsUpdate: u32,
    primitiveRemapOffset: u32,
};

struct HitState {
    bestT: f32,
    bestPrimitive: u32,
    bestInstance: u32,
    bestBary: vec2<f32>,
    iterationCount: u32,
};

struct BLASCandidate {
    instanceIndex: u32,
    blasHandle: u32,
    nearT: f32,
    flags: u32,
};

struct TLASNodeMetadata {
    flags: u32,
    rotationIndex: u32,
    quantizedFlags: u32,
    padding: u32,
};

struct StreamTask {
    kind: u32,
    baseIndex: u32,
    mask: u32,
    instanceIndex: u32,
    blasHandle: u32,
    rayCount: u32,
    flags: u32,
    _padding0: u32,
    rayRefs: array<u32, 32>,
};

struct StreamCounters {
    taskCountA: atomic<u32>,
    taskCountB: atomic<u32>,
    localRayCount: atomic<u32>,
    overflowCount: atomic<u32>,
    hashOccupiedCount: atomic<u32>,
    hashBlockCount: atomic<u32>,
    hashInsertFailureCount: atomic<u32>,
    _padding0: atomic<u32>,
};

struct DispatchIndirectArgs {
    x: u32,
    y: u32,
    z: u32,
};

struct StreamLocalRay {
    rayId: u32,
    instanceIndex: u32,
    blasHandle: u32,
    _padding0: u32,
    localOriginTMin: vec4<f32>,
    localDirectionTMax: vec4<f32>,
};

struct StreamGlobalHashSlot {
    keyHash: atomic<u32>,
    generation: atomic<u32>,
    headBlock: atomic<u32>,
    tailBlock: atomic<u32>,
    kind: u32,
    baseIndex: u32,
    mask: u32,
    instanceIndex: u32,
    blasHandle: u32,
};

struct StreamPacketBlock {
    nextBlock: atomic<u32>,
    rayCount: u32,
    rayRefs: array<u32, 32>,
};

struct TraceStats {
    totalRays: atomic<u32>,
    activeRays: atomic<u32>,
    hitRays: atomic<u32>,
    missRays: atomic<u32>,
    invalidDirectionRays: atomic<u32>,
    tlasNodeVisits: atomic<u32>,
    blasNodeVisits: atomic<u32>,
    worldBlasNodeVisits: atomic<u32>,
    triangleTests: atomic<u32>,
    workgroupQueuePushes: atomic<u32>,
    workgroupQueuePops: atomic<u32>,
    localStackSpills: atomic<u32>,
    overlayNodeTests: atomic<u32>,
    overlayEdgeHits: atomic<u32>,
    lastHitPrimitive: atomic<u32>,
    lastHitInstance: atomic<u32>,
    lastHitValid: atomic<u32>,
    traversalEarlyOutSkips: atomic<u32>,
    debugPixelsReachedValidBLAS: atomic<u32>,
    debugPixelsReachedInvalidBLAS: atomic<u32>,
    debugPixelsVisitedBLASNode: atomic<u32>,
    debugPixelsReadTriangle: atomic<u32>,
    debugPixelsRejectedTriangle: atomic<u32>,
    debugPixelsAcceptedTriangle: atomic<u32>,
    debugPixelsRejectedWorldT: atomic<u32>,
    debugPixelsAcceptedCandidate: atomic<u32>,
    debugPixelsBLASLeafGroup: atomic<u32>,
    debugPixelsInvalidLeafRemap: atomic<u32>,
    debugPixelsInvalidLocalRay: atomic<u32>,
    dynamicRayFetchCursor: atomic<u32>,
    twoStageBLASCandidatesEmitted: atomic<u32>,
    twoStageBLASCandidateOverflows: atomic<u32>,
    twoStageBLASCandidateSorts: atomic<u32>,
    twoStageBLASCandidatesTested: atomic<u32>,
    twoStageBLASCandidateDistanceEarlyStops: atomic<u32>,
    twoStageMaxBLASCandidatesPerRay: atomic<u32>,
    streamHashSlotsFlattened: atomic<u32>,
    streamHashBlocksFlattened: atomic<u32>,
    streamHashPacketsEmitted: atomic<u32>,
    streamHashInsertFailures: atomic<u32>,
    streamQueueOverflows: atomic<u32>,
    streamFinalTaskCountA: atomic<u32>,
    streamFinalTaskCountB: atomic<u32>,
    streamFinalHashOccupiedCount: atomic<u32>,
    streamFinalHashBlockCount: atomic<u32>,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var<storage, read> tlasInstances: array<InstanceGPU>;
@group(0) @binding(2) var<storage, read> tlasNodes: array<vec4<u32>>;
@group(0) @binding(3) var<storage, read> tlasLeafInstances: array<u32>;
@group(0) @binding(9) var<storage, read> blasNodes: array<vec4<u32>>;
#ifdef SWRT_WORLD_CWBVH
@group(0) @binding(10) var<storage, read> blasLeafRemap: array<vec2<u32>>;
#else
@group(0) @binding(10) var<storage, read> blasLeafRemap: array<u32>;
#endif
@group(0) @binding(11) var<storage, read_write> traceStats: TraceStats;
@group(0) @binding(12) var outputTexture: texture_storage_2d<rgba16float, write>;
@group(0) @binding(13) var<storage, read_write> rayInput: array<TraceRay>;
@group(0) @binding(14) var<storage, read_write> hitOutput: array<TraceHit>;
@group(0) @binding(19) var<storage, read> tlasInstanceAabbs: array<InstanceAABBGPU>;
@group(0) @binding(20) var<storage, read> tlasBvh4Nodes: array<BVH4NodeGPU>;
#ifdef SWRT_WORLD_CWBVH
@group(0) @binding(21) var<storage, read> worldInstanceRanges: array<WorldInstanceRangeGPU>;
#endif
@group(0) @binding(22) var<storage, read_write> raySortBinCounts: array<atomic<u32>>;
@group(0) @binding(23) var<storage, read_write> raySortBinOffsets: array<u32>;
@group(0) @binding(24) var<storage, read_write> raySortIds: array<u32>;
@group(0) @binding(25) var<storage, read_write> blasCandidates: array<BLASCandidate>;
@group(0) @binding(26) var<storage, read_write> blasCandidateCounts: array<u32>;
@group(0) @binding(27) var<storage, read> tlasDobbNodes: array<vec4<u32>>;
@group(0) @binding(28) var<storage, read> tlasNodeMetadata: array<TLASNodeMetadata>;
@group(0) @binding(30) var<storage, read> blasDobbNodes: array<vec4<u32>>;
@group(0) @binding(31) var<storage, read> blasNodeMetadata: array<TLASNodeMetadata>;
@group(0) @binding(32) var<storage, read_write> streamTasksA: array<StreamTask>;
@group(0) @binding(33) var<storage, read_write> streamTasksB: array<StreamTask>;
@group(0) @binding(34) var<storage, read_write> streamCounters: StreamCounters;
@group(0) @binding(35) var<storage, read_write> streamLocalRays: array<StreamLocalRay>;
@group(0) @binding(36) var<storage, read_write> streamHitT: array<atomic<u32>>;
@group(0) @binding(37) var<storage, read_write> streamHashSlots: array<StreamGlobalHashSlot>;
@group(0) @binding(38) var<storage, read_write> streamPacketBlocks: array<StreamPacketBlock>;
@group(0) @binding(39) var<storage, read_write> streamHashOccupiedSlots: array<u32>;
@group(0) @binding(40) var<storage, read_write> streamIndirectArgs: array<DispatchIndirectArgs>;

var<workgroup> streamLocalBucketCounts: array<atomic<u32>, 128>;
var<workgroup> streamLocalBucketKinds: array<u32, 128>;
var<workgroup> streamLocalBucketBaseIndices: array<u32, 128>;
var<workgroup> streamLocalBucketMasks: array<u32, 128>;
var<workgroup> streamLocalBucketInstanceIndices: array<u32, 128>;
var<workgroup> streamLocalBucketBlasHandles: array<u32, 128>;
var<workgroup> streamLocalBucketRayRefs: array<u32, 4096>;
var<workgroup> streamWorkgroupQueueCount: u32;
var<workgroup> streamWorkgroupQueueReadCursor: u32;
var<workgroup> streamWorkgroupQueueBatchStart: u32;
var<workgroup> streamWorkgroupQueueBatchCount: u32;
var<workgroup> streamOutputToB: u32;
var<workgroup> streamWorkgroupQueueKinds: array<u32, 32>;
var<workgroup> streamWorkgroupQueueBaseIndices: array<u32, 32>;
var<workgroup> streamWorkgroupQueueMasks: array<u32, 32>;
var<workgroup> streamWorkgroupQueueInstanceIndices: array<u32, 32>;
var<workgroup> streamWorkgroupQueueBlasHandles: array<u32, 32>;
var<workgroup> streamWorkgroupQueueRayCounts: array<u32, 32>;
var<workgroup> streamWorkgroupQueueRayRefs: array<u32, 1024>;
var<workgroup> streamFlattenValid: u32;
var<workgroup> streamFlattenDone: u32;
var<workgroup> streamFlattenKind: u32;
var<workgroup> streamFlattenBaseIndex: u32;
var<workgroup> streamFlattenMask: u32;
var<workgroup> streamFlattenInstanceIndex: u32;
var<workgroup> streamFlattenBlasHandle: u32;
var<workgroup> streamFlattenBlockCursor: u32;
var<workgroup> streamFlattenBlockRayOffset: u32;
var<workgroup> streamFlattenBlockGuard: u32;
var<workgroup> streamFlattenScratchCount: u32;
var<workgroup> streamFlattenScratchRefs: array<u32, 256>;
var<workgroup> streamFlattenOutTaskIndices: array<u32, 8>;

fn load_local_leaf_remap(index: u32) -> u32 {
#ifdef SWRT_WORLD_CWBVH
    return blasLeafRemap[index].x;
#else
    return blasLeafRemap[index];
#endif
}

fn trace_stat_add_local_stack_spill(value: u32) {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.localStackSpills, value);
#endif
}

fn trace_stat_add_tlas_node_visit(value: u32) {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.tlasNodeVisits, value);
#endif
}

fn trace_stat_add_blas_node_visit(value: u32) {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.blasNodeVisits, value);
#endif
}

fn trace_stat_add_world_blas_node_visit(value: u32) {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.blasNodeVisits, value);
    atomicAdd(&traceStats.worldBlasNodeVisits, value);
#endif
}

fn trace_stat_add_triangle_test(value: u32) {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.triangleTests, value);
#endif
}

fn trace_stat_add_workgroup_queue_push() {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.workgroupQueuePushes, 1u);
#endif
}

fn trace_stat_add_workgroup_queue_pop() {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.workgroupQueuePops, 1u);
#endif
}

fn trace_stat_begin_ray() {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.totalRays, 1u);
#endif
}

fn trace_stat_begin_valid_ray() {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.activeRays, 1u);
#endif
}

fn trace_stat_add_invalid_ray() {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.invalidDirectionRays, 1u);
    atomicAdd(&traceStats.missRays, 1u);
#endif
}

fn trace_stat_add_miss() {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.missRays, 1u);
#endif
}

fn trace_stat_add_hit(primitiveId: u32, instanceId: u32) {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.hitRays, 1u);
    atomicStore(&traceStats.lastHitPrimitive, primitiveId);
    atomicStore(&traceStats.lastHitInstance, instanceId);
    atomicStore(&traceStats.lastHitValid, 1u);
#endif
}

fn trace_stat_add_two_stage_candidates_emitted(value: u32) {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.twoStageBLASCandidatesEmitted, value);
#endif
}

fn trace_stat_add_two_stage_candidate_overflow() {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.twoStageBLASCandidateOverflows, 1u);
#endif
}

fn trace_stat_add_two_stage_candidate_sort() {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.twoStageBLASCandidateSorts, 1u);
#endif
}

fn trace_stat_add_two_stage_blas_candidate_test() {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.twoStageBLASCandidatesTested, 1u);
#endif
}

fn trace_stat_add_two_stage_distance_early_stop() {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.twoStageBLASCandidateDistanceEarlyStops, 1u);
#endif
}

fn trace_stat_update_two_stage_max_candidates(value: u32) {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicMax(&traceStats.twoStageMaxBLASCandidatesPerRay, value);
#endif
}

fn trace_stat_add_stream_hash_slot_flattened() {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.streamHashSlotsFlattened, 1u);
#endif
}

fn trace_stat_add_stream_hash_blocks_flattened(value: u32) {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.streamHashBlocksFlattened, value);
#endif
}

fn trace_stat_add_stream_hash_packet_emitted() {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.streamHashPacketsEmitted, 1u);
#endif
}

fn trace_stat_add_stream_hash_insert_failure() {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicAdd(&traceStats.streamHashInsertFailures, 1u);
#endif
}

fn trace_stat_store_stream_final_counters() {
#ifdef SWRT_ENABLE_TRACE_STATS
    atomicStore(&traceStats.streamQueueOverflows, atomicLoad(&streamCounters.overflowCount));
    atomicStore(&traceStats.streamFinalTaskCountA, atomicLoad(&streamCounters.taskCountA));
    atomicStore(&traceStats.streamFinalTaskCountB, atomicLoad(&streamCounters.taskCountB));
    atomicStore(&traceStats.streamFinalHashOccupiedCount, atomicLoad(&streamCounters.hashOccupiedCount));
    atomicStore(&traceStats.streamFinalHashBlockCount, atomicLoad(&streamCounters.hashBlockCount));
#endif
}

@compute @workgroup_size(1, 1, 1)
fn clear_stats() {
    atomicStore(&traceStats.totalRays, 0u);
    atomicStore(&traceStats.activeRays, 0u);
    atomicStore(&traceStats.hitRays, 0u);
    atomicStore(&traceStats.missRays, 0u);
    atomicStore(&traceStats.invalidDirectionRays, 0u);
    atomicStore(&traceStats.tlasNodeVisits, 0u);
    atomicStore(&traceStats.blasNodeVisits, 0u);
    atomicStore(&traceStats.worldBlasNodeVisits, 0u);
    atomicStore(&traceStats.triangleTests, 0u);
    atomicStore(&traceStats.workgroupQueuePushes, 0u);
    atomicStore(&traceStats.workgroupQueuePops, 0u);
    atomicStore(&traceStats.localStackSpills, 0u);
    atomicStore(&traceStats.overlayNodeTests, 0u);
    atomicStore(&traceStats.overlayEdgeHits, 0u);
    atomicStore(&traceStats.lastHitPrimitive, INVALID_INDEX);
    atomicStore(&traceStats.lastHitInstance, INVALID_INDEX);
    atomicStore(&traceStats.lastHitValid, 0u);
    atomicStore(&traceStats.traversalEarlyOutSkips, 0u);
    atomicStore(&traceStats.debugPixelsReachedValidBLAS, 0u);
    atomicStore(&traceStats.debugPixelsReachedInvalidBLAS, 0u);
    atomicStore(&traceStats.debugPixelsVisitedBLASNode, 0u);
    atomicStore(&traceStats.debugPixelsReadTriangle, 0u);
    atomicStore(&traceStats.debugPixelsRejectedTriangle, 0u);
    atomicStore(&traceStats.debugPixelsAcceptedTriangle, 0u);
    atomicStore(&traceStats.debugPixelsRejectedWorldT, 0u);
    atomicStore(&traceStats.debugPixelsAcceptedCandidate, 0u);
    atomicStore(&traceStats.debugPixelsBLASLeafGroup, 0u);
    atomicStore(&traceStats.debugPixelsInvalidLeafRemap, 0u);
    atomicStore(&traceStats.debugPixelsInvalidLocalRay, 0u);
    atomicStore(&traceStats.dynamicRayFetchCursor, 0u);
    atomicStore(&traceStats.twoStageBLASCandidatesEmitted, 0u);
    atomicStore(&traceStats.twoStageBLASCandidateOverflows, 0u);
    atomicStore(&traceStats.twoStageBLASCandidateSorts, 0u);
    atomicStore(&traceStats.twoStageBLASCandidatesTested, 0u);
    atomicStore(&traceStats.twoStageBLASCandidateDistanceEarlyStops, 0u);
    atomicStore(&traceStats.twoStageMaxBLASCandidatesPerRay, 0u);
    atomicStore(&traceStats.streamHashSlotsFlattened, 0u);
    atomicStore(&traceStats.streamHashBlocksFlattened, 0u);
    atomicStore(&traceStats.streamHashPacketsEmitted, 0u);
    atomicStore(&traceStats.streamHashInsertFailures, 0u);
    atomicStore(&traceStats.streamQueueOverflows, 0u);
    atomicStore(&traceStats.streamFinalTaskCountA, 0u);
    atomicStore(&traceStats.streamFinalTaskCountB, 0u);
    atomicStore(&traceStats.streamFinalHashOccupiedCount, 0u);
    atomicStore(&traceStats.streamFinalHashBlockCount, 0u);
    atomicStore(&streamCounters.taskCountA, 0u);
    atomicStore(&streamCounters.taskCountB, 0u);
    atomicStore(&streamCounters.localRayCount, 0u);
    atomicStore(&streamCounters.overflowCount, 0u);
    atomicStore(&streamCounters.hashOccupiedCount, 0u);
    atomicStore(&streamCounters.hashBlockCount, 0u);
    atomicStore(&streamCounters.hashInsertFailureCount, 0u);
    streamIndirectArgs[0u] = DispatchIndirectArgs(0u, 1u, 1u);
}

@compute @workgroup_size(64, 1, 1)
fn stream_clear_task_a(@builtin(global_invocation_id) globalId: vec3<u32>) {
    if globalId.x == 0u {
        atomicStore(&streamCounters.taskCountA, 0u);
        atomicStore(&streamCounters.hashOccupiedCount, 0u);
        atomicStore(&streamCounters.hashBlockCount, 0u);
        atomicStore(&streamCounters.hashInsertFailureCount, 0u);
    }
}

@compute @workgroup_size(64, 1, 1)
fn stream_clear_task_b(@builtin(global_invocation_id) globalId: vec3<u32>) {
    if globalId.x == 0u {
        atomicStore(&streamCounters.taskCountB, 0u);
        atomicStore(&streamCounters.hashOccupiedCount, 0u);
        atomicStore(&streamCounters.hashBlockCount, 0u);
        atomicStore(&streamCounters.hashInsertFailureCount, 0u);
    }
}

@compute @workgroup_size(64, 1, 1)
fn stream_clear_hash_slots(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let slot = globalId.x;
    if slot < stream_hash_slot_capacity() {
        atomicStore(&streamHashSlots[slot].keyHash, STREAM_GLOBAL_HASH_EMPTY);
        atomicStore(&streamHashSlots[slot].generation, 0u);
        atomicStore(&streamHashSlots[slot].headBlock, INVALID_INDEX);
        atomicStore(&streamHashSlots[slot].tailBlock, INVALID_INDEX);
    }
}

fn stream_seed_ray_state(rayId: u32) -> bool {
    if rayId >= uniforms.rayCount {
        return false;
    }

    trace_stat_begin_ray();
    atomicStore(&streamHitT[rayId], bitcast<u32>(uniforms.tMax));
    hitOutput[rayId] = invalid_trace_hit();

    let ray = rayInput[rayId];
    if trace_ray_is_valid(ray) {
        trace_stat_begin_valid_ray();
        return true;
    }

    trace_stat_add_invalid_ray();
    write_hit_output(rayId, DEBUG_HIT_INSTANCE_INVALID_RAY, 0u, 0.0, vec2<f32>(0.0), INVALID_INDEX, 0u);
    return false;
}

@compute @workgroup_size(64, 1, 1)
fn stream_seed_tlas_root_packets(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let rayId = globalId.x;
    if rayId == 0u {
        atomicStore(&streamCounters.taskCountA, (uniforms.rayCount + STREAM_PACKET_RAY_CAPACITY - 1u) / STREAM_PACKET_RAY_CAPACITY);
        atomicStore(&streamCounters.taskCountB, 0u);
        atomicStore(&streamCounters.localRayCount, 0u);
        atomicStore(&streamCounters.overflowCount, 0u);
        atomicStore(&streamCounters.hashOccupiedCount, 0u);
        atomicStore(&streamCounters.hashBlockCount, 0u);
        atomicStore(&streamCounters.hashInsertFailureCount, 0u);
    }
    if rayId >= uniforms.rayCount {
        return;
    }

    trace_stat_begin_ray();
    let packetIndex = rayId / STREAM_PACKET_RAY_CAPACITY;
    let lane = rayId % STREAM_PACKET_RAY_CAPACITY;
    if lane == 0u {
        streamTasksA[packetIndex].kind = STREAM_TASK_KIND_TLAS_GROUP;
        streamTasksA[packetIndex].baseIndex = 0u;
        streamTasksA[packetIndex].mask = 0x80000000u;
        streamTasksA[packetIndex].instanceIndex = INVALID_INDEX;
        streamTasksA[packetIndex].blasHandle = INVALID_INDEX;
        streamTasksA[packetIndex].rayCount = min(STREAM_PACKET_RAY_CAPACITY, uniforms.rayCount - packetIndex * STREAM_PACKET_RAY_CAPACITY);
        streamTasksA[packetIndex].flags = 0u;
        streamTasksA[packetIndex]._padding0 = 0u;
    }
    streamTasksA[packetIndex].rayRefs[lane] = rayId;
    _ = stream_seed_ray_state(rayId);
}

@compute @workgroup_size(128, 1, 1)
fn stream_seed_trace_tlas_root_to_b(@builtin(workgroup_id) workgroupId: vec3<u32>, @builtin(local_invocation_id) localId: vec3<u32>) {
    let localIndex = localId.x;
    let workgroupTaskBase = workgroupId.x * STREAM_PACKETS_PER_WORKGROUP;
    let packetInWorkgroup = localIndex / STREAM_PACKET_RAY_CAPACITY;
    let lane = localIndex % STREAM_PACKET_RAY_CAPACITY;
    let packetIndex = workgroupTaskBase + packetInWorkgroup;
    let rayId = packetIndex * STREAM_PACKET_RAY_CAPACITY + lane;
    let bucketBase = packetInWorkgroup * STREAM_BUCKETS_PER_PACKET;
    stream_reset_workgroup_queue(localIndex);
    stream_clear_local_buckets(localIndex);
    if localIndex == 0u {
        streamOutputToB = 1u;
    }
    workgroupBarrier();

    if rayId < uniforms.rayCount {
        if lane == 0u {
            trace_stat_add_workgroup_queue_pop();
        }
        if stream_seed_ray_state(rayId) && uniforms.tlasNodeCount != 0u {
            stream_process_tlas_task(0u, 0x80000000u, rayId, rayInput[rayId], bucketBase);
        }
    }
    workgroupBarrier();
    if localIndex == 0u {
        stream_flush_local_buckets_to_workgroup_or_b();
    }
    workgroupBarrier();

    let localIterationCount = stream_max_local_node_iterations();
    for (var localIteration = 0u; localIteration < localIterationCount; localIteration += 1u) {
        stream_clear_local_buckets(localIndex);
        if localIndex == 0u {
            stream_prepare_workgroup_queue_batch();
        }
        workgroupBarrier();
        if streamWorkgroupQueueBatchCount == 0u {
            break;
        }
        stream_process_workgroup_queue_batch(localIndex);
        workgroupBarrier();
        if localIndex == 0u {
            stream_flush_local_buckets_to_workgroup_or_b();
        }
        workgroupBarrier();
    }

    if localIndex == 0u {
        stream_flush_remaining_workgroup_queue_to_b();
    }
}

fn stream_prepare_indirect_args_for_count(count: u32, itemsPerWorkgroup: u32) {
    let cappedCount = min(count, stream_task_capacity());
    let groupCount = select(0u, (cappedCount + itemsPerWorkgroup - 1u) / itemsPerWorkgroup, cappedCount != 0u);
    streamIndirectArgs[0u] = DispatchIndirectArgs(groupCount, 1u, 1u);
}

@compute @workgroup_size(1, 1, 1)
fn stream_prepare_trace_a_indirect() {
    atomicStore(&streamCounters.taskCountB, 0u);
    atomicStore(&streamCounters.hashOccupiedCount, 0u);
    atomicStore(&streamCounters.hashBlockCount, 0u);
    atomicStore(&streamCounters.hashInsertFailureCount, 0u);
    stream_prepare_indirect_args_for_count(atomicLoad(&streamCounters.taskCountA), STREAM_PACKETS_PER_WORKGROUP);
}

@compute @workgroup_size(1, 1, 1)
fn stream_prepare_trace_b_indirect() {
    atomicStore(&streamCounters.taskCountA, 0u);
    atomicStore(&streamCounters.hashOccupiedCount, 0u);
    atomicStore(&streamCounters.hashBlockCount, 0u);
    atomicStore(&streamCounters.hashInsertFailureCount, 0u);
    stream_prepare_indirect_args_for_count(atomicLoad(&streamCounters.taskCountB), STREAM_PACKETS_PER_WORKGROUP);
}

@compute @workgroup_size(1, 1, 1)
fn stream_prepare_hash_indirect() {
    let occupiedCount = min(atomicLoad(&streamCounters.hashOccupiedCount), stream_hash_slot_capacity());
    streamIndirectArgs[0u] = DispatchIndirectArgs(occupiedCount, 1u, 1u);
}

fn stream_trace_task_to_b(workgroupTaskBase: u32, localIndex: u32) {
    let currentCount = atomicLoad(&streamCounters.taskCountA);
    let packetInWorkgroup = localIndex / STREAM_PACKET_RAY_CAPACITY;
    let lane = localIndex % STREAM_PACKET_RAY_CAPACITY;
    let taskIndex = workgroupTaskBase + packetInWorkgroup;
    let bucketBase = packetInWorkgroup * STREAM_BUCKETS_PER_PACKET;
    stream_reset_workgroup_queue(localIndex);
    stream_clear_local_buckets(localIndex);
    if localIndex == 0u {
        streamOutputToB = 1u;
    }
    workgroupBarrier();
    if taskIndex < currentCount && taskIndex < stream_task_capacity() {
        let taskKind = streamTasksA[taskIndex].kind;
        let taskBaseIndex = streamTasksA[taskIndex].baseIndex;
        let taskMask = streamTasksA[taskIndex].mask;
        let taskRayCount = streamTasksA[taskIndex].rayCount;
        if lane == 0u {
            trace_stat_add_workgroup_queue_pop();
        }
        if lane < taskRayCount {
            stream_process_task_lane(
                taskKind,
                taskBaseIndex,
                taskMask,
                streamTasksA[taskIndex].instanceIndex,
                streamTasksA[taskIndex].blasHandle,
                streamTasksA[taskIndex].rayRefs[lane],
                bucketBase);
        }
    }
    workgroupBarrier();
    if localIndex == 0u {
        stream_flush_local_buckets_to_workgroup_or_b();
    }
    workgroupBarrier();

    let localIterationCount = stream_max_local_node_iterations();
    for (var localIteration = 0u; localIteration < localIterationCount; localIteration += 1u) {
        stream_clear_local_buckets(localIndex);
        if localIndex == 0u {
            stream_prepare_workgroup_queue_batch();
        }
        workgroupBarrier();
        if streamWorkgroupQueueBatchCount == 0u {
            break;
        }
        stream_process_workgroup_queue_batch(localIndex);
        workgroupBarrier();
        if localIndex == 0u {
            stream_flush_local_buckets_to_workgroup_or_b();
        }
        workgroupBarrier();
    }

    if localIndex == 0u {
        stream_flush_remaining_workgroup_queue_to_b();
    }
}

fn stream_trace_task_to_a(workgroupTaskBase: u32, localIndex: u32) {
    let currentCount = atomicLoad(&streamCounters.taskCountB);
    let packetInWorkgroup = localIndex / STREAM_PACKET_RAY_CAPACITY;
    let lane = localIndex % STREAM_PACKET_RAY_CAPACITY;
    let taskIndex = workgroupTaskBase + packetInWorkgroup;
    let bucketBase = packetInWorkgroup * STREAM_BUCKETS_PER_PACKET;
    stream_reset_workgroup_queue(localIndex);
    stream_clear_local_buckets(localIndex);
    if localIndex == 0u {
        streamOutputToB = 0u;
    }
    workgroupBarrier();
    if taskIndex < currentCount && taskIndex < stream_task_capacity() {
        let taskKind = streamTasksB[taskIndex].kind;
        let taskBaseIndex = streamTasksB[taskIndex].baseIndex;
        let taskMask = streamTasksB[taskIndex].mask;
        let taskRayCount = streamTasksB[taskIndex].rayCount;
        if lane == 0u {
            trace_stat_add_workgroup_queue_pop();
        }
        if lane < taskRayCount {
            stream_process_task_lane(
                taskKind,
                taskBaseIndex,
                taskMask,
                streamTasksB[taskIndex].instanceIndex,
                streamTasksB[taskIndex].blasHandle,
                streamTasksB[taskIndex].rayRefs[lane],
                bucketBase);
        }
    }
    workgroupBarrier();
    if localIndex == 0u {
        stream_flush_local_buckets_to_workgroup_or_a();
    }
    workgroupBarrier();

    let localIterationCount = stream_max_local_node_iterations();
    for (var localIteration = 0u; localIteration < localIterationCount; localIteration += 1u) {
        stream_clear_local_buckets(localIndex);
        if localIndex == 0u {
            stream_prepare_workgroup_queue_batch();
        }
        workgroupBarrier();
        if streamWorkgroupQueueBatchCount == 0u {
            break;
        }
        stream_process_workgroup_queue_batch(localIndex);
        workgroupBarrier();
        if localIndex == 0u {
            stream_flush_local_buckets_to_workgroup_or_a();
        }
        workgroupBarrier();
    }

    if localIndex == 0u {
        stream_flush_remaining_workgroup_queue_to_a();
    }
}

@compute @workgroup_size(128, 1, 1)
fn stream_trace_a_to_b(@builtin(workgroup_id) workgroupId: vec3<u32>, @builtin(local_invocation_id) localId: vec3<u32>) {
    stream_trace_task_to_b(workgroupId.x * STREAM_PACKETS_PER_WORKGROUP, localId.x);
}

@compute @workgroup_size(128, 1, 1)
fn stream_trace_b_to_a(@builtin(workgroup_id) workgroupId: vec3<u32>, @builtin(local_invocation_id) localId: vec3<u32>) {
    stream_trace_task_to_a(workgroupId.x * STREAM_PACKETS_PER_WORKGROUP, localId.x);
}

fn stream_write_flatten_task_header(toB: bool, index: u32, rayCount: u32) -> bool {
    if index >= stream_task_capacity() {
        stream_mark_overflow();
        return false;
    }
    if toB {
        streamTasksB[index].kind = streamFlattenKind;
        streamTasksB[index].baseIndex = streamFlattenBaseIndex;
        streamTasksB[index].mask = streamFlattenMask;
        streamTasksB[index].instanceIndex = streamFlattenInstanceIndex;
        streamTasksB[index].blasHandle = streamFlattenBlasHandle;
        streamTasksB[index].rayCount = rayCount;
        streamTasksB[index].flags = 0u;
        streamTasksB[index]._padding0 = 0u;
    } else {
        streamTasksA[index].kind = streamFlattenKind;
        streamTasksA[index].baseIndex = streamFlattenBaseIndex;
        streamTasksA[index].mask = streamFlattenMask;
        streamTasksA[index].instanceIndex = streamFlattenInstanceIndex;
        streamTasksA[index].blasHandle = streamFlattenBlasHandle;
        streamTasksA[index].rayCount = rayCount;
        streamTasksA[index].flags = 0u;
        streamTasksA[index]._padding0 = 0u;
    }
    return true;
}

fn stream_write_flatten_task_ref(toB: bool, taskIndex: u32, rayLane: u32, rayRef: u32) {
    if toB {
        streamTasksB[taskIndex].rayRefs[rayLane] = rayRef;
    } else {
        streamTasksA[taskIndex].rayRefs[rayLane] = rayRef;
    }
}

fn stream_flatten_initialize_slot(occupiedIndex: u32) {
    streamFlattenValid = 0u;
    streamFlattenDone = 1u;
    streamFlattenScratchCount = 0u;
    streamFlattenBlockCursor = INVALID_INDEX;
    streamFlattenBlockRayOffset = 0u;
    streamFlattenBlockGuard = 0u;

    let occupiedCount = min(atomicLoad(&streamCounters.hashOccupiedCount), stream_hash_slot_capacity());
    if occupiedIndex >= occupiedCount {
        return;
    }
    let slot = streamHashOccupiedSlots[occupiedIndex];
    if slot >= stream_hash_slot_capacity() {
        return;
    }
    if atomicLoad(&streamHashSlots[slot].generation) != stream_hash_generation() {
        return;
    }
    let keyHash = atomicLoad(&streamHashSlots[slot].keyHash);
    if keyHash == STREAM_GLOBAL_HASH_EMPTY || keyHash == STREAM_GLOBAL_HASH_CLAIMING {
        return;
    }

    streamFlattenKind = streamHashSlots[slot].kind;
    streamFlattenBaseIndex = streamHashSlots[slot].baseIndex;
    streamFlattenMask = streamHashSlots[slot].mask;
    streamFlattenInstanceIndex = streamHashSlots[slot].instanceIndex;
    streamFlattenBlasHandle = streamHashSlots[slot].blasHandle;
    streamFlattenBlockCursor = atomicLoad(&streamHashSlots[slot].headBlock);
    streamFlattenBlockRayOffset = 0u;
    streamFlattenBlockGuard = 0u;
    streamFlattenDone = select(0u, 1u, streamFlattenBlockCursor == INVALID_INDEX || streamFlattenBlockCursor >= stream_hash_block_capacity());
    streamFlattenValid = 1u;
    trace_stat_add_stream_hash_slot_flattened();

    atomicStore(&streamHashSlots[slot].headBlock, INVALID_INDEX);
    atomicStore(&streamHashSlots[slot].tailBlock, INVALID_INDEX);
    atomicStore(&streamHashSlots[slot].keyHash, STREAM_GLOBAL_HASH_EMPTY);
    atomicStore(&streamHashSlots[slot].generation, 0u);
}

fn stream_flatten_fill_scratch() {
    var count = 0u;
    var block = streamFlattenBlockCursor;
    var rayOffset = streamFlattenBlockRayOffset;
    var guard = streamFlattenBlockGuard;
    var blocksFlattened = 0u;
    let blockCapacity = stream_hash_block_capacity();

    while count < STREAM_FLATTEN_SCRATCH_REF_CAPACITY && block != INVALID_INDEX && block < blockCapacity && guard < blockCapacity {
        let rayCount = min(streamPacketBlocks[block].rayCount, STREAM_PACKET_RAY_CAPACITY);
        var rayIndex = min(rayOffset, rayCount);
        while rayIndex < rayCount && count < STREAM_FLATTEN_SCRATCH_REF_CAPACITY {
            streamFlattenScratchRefs[count] = streamPacketBlocks[block].rayRefs[rayIndex];
            count += 1u;
            rayIndex += 1u;
        }

        if rayIndex >= rayCount {
            block = atomicLoad(&streamPacketBlocks[block].nextBlock);
            rayOffset = 0u;
            guard += 1u;
            blocksFlattened += 1u;
        } else {
            rayOffset = rayIndex;
        }
    }

    streamFlattenScratchCount = count;
    streamFlattenBlockCursor = block;
    streamFlattenBlockRayOffset = rayOffset;
    streamFlattenBlockGuard = guard;
    streamFlattenDone = select(0u, 1u, block == INVALID_INDEX || block >= blockCapacity || guard >= blockCapacity);
    if blocksFlattened != 0u {
        trace_stat_add_stream_hash_blocks_flattened(blocksFlattened);
    }
}

fn stream_flatten_emit_scratch(toB: bool, localIndex: u32) {
    let scratchCount = streamFlattenScratchCount;
    let fullPacketCount = scratchCount / STREAM_PACKET_RAY_CAPACITY;
    let tailCount = scratchCount - fullPacketCount * STREAM_PACKET_RAY_CAPACITY;
    let tailPacketCount = select(0u, 1u, streamFlattenDone != 0u && tailCount != 0u);
    let emitPacketCount = fullPacketCount + tailPacketCount;
    let emitRefCount = fullPacketCount * STREAM_PACKET_RAY_CAPACITY + select(0u, tailCount, tailPacketCount != 0u);

    if localIndex < emitPacketCount {
        let rayCount = select(STREAM_PACKET_RAY_CAPACITY, tailCount, tailPacketCount != 0u && localIndex == fullPacketCount);
        var outIndex = 0u;
        if toB {
            outIndex = atomicAdd(&streamCounters.taskCountB, 1u);
        } else {
            outIndex = atomicAdd(&streamCounters.taskCountA, 1u);
        }
        streamFlattenOutTaskIndices[localIndex] = select(INVALID_INDEX, outIndex, stream_write_flatten_task_header(toB, outIndex, rayCount));
        trace_stat_add_stream_hash_packet_emitted();
    }
    workgroupBarrier();

    for (var refIndex = localIndex; refIndex < emitRefCount; refIndex += 64u) {
        let packetIndex = refIndex / STREAM_PACKET_RAY_CAPACITY;
        let rayLane = refIndex - packetIndex * STREAM_PACKET_RAY_CAPACITY;
        let outIndex = streamFlattenOutTaskIndices[packetIndex];
        if outIndex != INVALID_INDEX {
            stream_write_flatten_task_ref(toB, outIndex, rayLane, streamFlattenScratchRefs[refIndex]);
        }
    }
}

fn stream_flatten_hash_slot(occupiedIndex: u32, localIndex: u32, toB: bool) {
    if localIndex == 0u {
        stream_flatten_initialize_slot(occupiedIndex);
    }
    workgroupBarrier();
    if streamFlattenValid == 0u {
        return;
    }

    loop {
        if localIndex == 0u {
            stream_flatten_fill_scratch();
        }
        workgroupBarrier();
        stream_flatten_emit_scratch(toB, localIndex);
        workgroupBarrier();
        let done = streamFlattenDone != 0u;
        if done {
            break;
        }
    }
}

@compute @workgroup_size(64, 1, 1)
fn stream_flatten_hash_to_b(@builtin(workgroup_id) workgroupId: vec3<u32>, @builtin(local_invocation_id) localId: vec3<u32>) {
    stream_flatten_hash_slot(workgroupId.x, localId.x, true);
}

@compute @workgroup_size(64, 1, 1)
fn stream_flatten_hash_to_a(@builtin(workgroup_id) workgroupId: vec3<u32>, @builtin(local_invocation_id) localId: vec3<u32>) {
    stream_flatten_hash_slot(workgroupId.x, localId.x, false);
}

@compute @workgroup_size(64, 1, 1)
fn stream_finalize_hits(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let rayId = globalId.x;
    if rayId == 0u {
        trace_stat_store_stream_final_counters();
    }
    if rayId >= uniforms.rayCount {
        return;
    }
    let ray = rayInput[rayId];
    if !trace_ray_is_valid(ray) {
        return;
    }
    let hit = hitOutput[rayId];
    if hit.instanceId != INVALID_INDEX && hit.triangleId != INVALID_INDEX {
        trace_stat_add_hit(hit.triangleId, hit.tlasInstanceIndex);
    } else {
        trace_stat_add_miss();
        if debug_mode_heatmap() {
            write_hit_output(rayId, DEBUG_HIT_INSTANCE_HEATMAP, uniforms.heatmapMaxIterations, 0.0, vec2<f32>(0.0), INVALID_INDEX, 0u);
        }
    }
}

fn unpack_byte(word: u32, byte_index: u32) -> u32 {
    return (word >> (byte_index * 8u)) & 0xffu;
}

fn extract_byte(word: u32, byte_index: u32) -> u32 {
    return (word >> (byte_index * 8u)) & 0xffu;
}

fn most_significant_bit_index(mask: u32) -> u32 {
    return 31u - countLeadingZeros(mask);
}

fn decode_signed_u8(value: u32) -> i32 {
    return select(i32(value), i32(value) - 256, value >= 128u);
}

fn safe_reciprocal_direction_component(value: f32) -> f32 {
    let epsilon = exp2(-80.0);
    let safeValue = select(select(-epsilon, epsilon, value >= 0.0), value, abs(value) > epsilon);
    return 1.0 / safeValue;
}

fn safe_inverse_direction(direction: vec3<f32>) -> vec3<f32> {
    return vec3<f32>(
        safe_reciprocal_direction_component(direction.x),
        safe_reciprocal_direction_component(direction.y),
        safe_reciprocal_direction_component(direction.z));
}

fn traversal_early_out_enabled() -> bool {
    return uniforms.traceControl.x > 0.5;
}

fn occlusion_trace_enabled() -> bool {
    return uniforms.traceFlags.x != 0u;
}

fn should_skip_group_fast(groupMinT: f32, bestT: f32, earlyOut: bool, earlyOutBias: f32) -> bool {
    return earlyOut && bestT < uniforms.tMax && groupMinT > bestT + earlyOutBias;
}

fn sign_extend_s8x4(x: u32) -> u32 {
    return ((x >> 7u) & 0x01010101u) * 0xffu;
}

#ifdef SWRT_WORLD_CWBVH_SUBGROUP
const SWRT_TRIANGLE_POSTPONING_THRESHOLD_DIVISOR = 5u;

@diagnostic(off, subgroup_uniformity)
fn swrt_subgroup_ballot(pred: bool) -> vec4<u32> {
    return subgroupBallot(pred);
}

@diagnostic(off, subgroup_uniformity)
fn swrt_subgroup_broadcast_first_u32(value: u32) -> u32 {
    return subgroupBroadcastFirst(value);
}

fn swrt_subgroup_ballot_count(mask: vec4<u32>) -> u32 {
    return countOneBits(mask.x) +
        countOneBits(mask.y) +
        countOneBits(mask.z) +
        countOneBits(mask.w);
}

fn swrt_subgroup_active_lane_count(pred: bool) -> u32 {
    return swrt_subgroup_ballot_count(swrt_subgroup_ballot(pred));
}
#endif

fn push_tlas_stack_entry(
    traversalStack: ptr<function, array<vec4<u32>, TLAS_TRAVERSAL_STACK_SIZE>>,
    stackPtr: ptr<function, u32>,
    baseIndex: u32,
    mask: u32,
    minT: f32) -> bool {
    if (*stackPtr) >= TLAS_TRAVERSAL_STACK_SIZE {
        trace_stat_add_local_stack_spill(1u);
        return true;
    }
    (*traversalStack)[(*stackPtr)] = vec4<u32>(baseIndex, mask, bitcast<u32>(minT), 0u);
    (*stackPtr) += 1u;
    return true;
}

fn pop_nearest_stack_entry(
    traversalStack: ptr<function, array<vec4<u32>, TLAS_TRAVERSAL_STACK_SIZE>>,
    stackPtr: ptr<function, u32>,
    maxT: f32,
    bestT: f32,
    earlyOut: bool,
    earlyOutBias: f32) -> vec4<u32> {
    var bestSlot = INVALID_INDEX;
    var bestMinT = maxT;
    for (var slot = 0u; slot < TLAS_TRAVERSAL_STACK_SIZE; slot += 1u) {
        if slot >= (*stackPtr) {
            break;
        }
        let entry = (*traversalStack)[slot];
        let entryMinT = bitcast<f32>(entry.z);
        if entry.y == 0u || entryMinT > maxT || should_skip_group_fast(entryMinT, bestT, earlyOut, earlyOutBias) {
            continue;
        }
        if bestSlot == INVALID_INDEX || entryMinT < bestMinT {
            bestSlot = slot;
            bestMinT = entryMinT;
        }
    }

    if bestSlot == INVALID_INDEX {
        (*stackPtr) = 0u;
        return vec4<u32>(0u);
    }

    let entry = (*traversalStack)[bestSlot];
    (*stackPtr) -= 1u;
    (*traversalStack)[bestSlot] = (*traversalStack)[(*stackPtr)];
    return entry;
}

fn hit_tie_epsilon(a: f32, b: f32) -> f32 {
    return max(max(abs(a), abs(b)) * 1.0e-7, 1.0e-6);
}

fn hit_candidate_is_better(candidateT: f32, candidatePrimitive: u32, bestT: f32, bestPrimitive: u32) -> bool {
    let epsilon = hit_tie_epsilon(candidateT, bestT);
    if candidateT < bestT - epsilon {
        return true;
    }
    if abs(candidateT - bestT) <= epsilon && (bestPrimitive == INVALID_INDEX || candidatePrimitive < bestPrimitive) {
        return true;
    }
    return false;
}

fn update_best_hit_local(state: ptr<function, HitState>, candidateT: f32, bary: vec2<f32>, primitiveIndex: u32, instanceIndex: u32) {
    if !hit_candidate_is_better(candidateT, primitiveIndex, (*state).bestT, (*state).bestPrimitive) {
        return;
    }
    (*state).bestT = candidateT;
    (*state).bestPrimitive = primitiveIndex;
    (*state).bestInstance = instanceIndex;
    (*state).bestBary = bary;
}

fn write_hit_output(rayId: u32, instanceId: u32, triangleId: u32, hitT: f32, bary: vec2<f32>, tlasInstanceIndex: u32, flags: u32) {
    if rayId >= uniforms.rayCount {
        return;
    }
    hitOutput[rayId] = TraceHit(instanceId, triangleId, hitT, bary.x, bary.y, tlasInstanceIndex, flags, 0u);
}

fn debug_mode_rbvh_card_aabb() -> bool {
    return uniforms.debugMode == DEBUG_MODE_RBVH_CARD_AABB;
}

fn debug_mode_heatmap() -> bool {
    return uniforms.debugMode == DEBUG_MODE_HEATMAP ||
        uniforms.debugMode == DEBUG_MODE_PRIMARY_RAY_HEATMAP;
}

fn mark_debug_rbvh_card_aabb(state: ptr<function, HitState>) {
    if debug_mode_rbvh_card_aabb() && (*state).bestInstance == INVALID_INDEX && (*state).bestPrimitive == INVALID_INDEX {
        (*state).bestPrimitive = DEBUG_HIT_RBVH_CARD_AABB;
    }
}

fn store_trace_result(rayId: u32, hitState: HitState) {
    let hitInstance = hitState.bestInstance;
    if hitInstance != INVALID_INDEX {
        trace_stat_add_hit(hitState.bestPrimitive, hitInstance);
    } else {
        trace_stat_add_miss();
    }

    if debug_mode_heatmap() {
        write_hit_output(rayId, DEBUG_HIT_INSTANCE_HEATMAP, min(hitState.iterationCount, uniforms.heatmapMaxIterations), 0.0, vec2<f32>(0.0), INVALID_INDEX, 0u);
        return;
    }

    if hitInstance != INVALID_INDEX {
#ifdef SWRT_WORLD_CWBVH
        let range = worldInstanceRanges[hitInstance];
        write_hit_output(rayId, range.sceneInstanceId, hitState.bestPrimitive, hitState.bestT, hitState.bestBary, range.instanceIndex, 1u);
#else
        let sceneInstanceId = tlasInstances[hitInstance].sceneInstanceId;
        write_hit_output(rayId, sceneInstanceId, hitState.bestPrimitive, hitState.bestT, hitState.bestBary, hitInstance, 1u);
#endif
    } else if debug_mode_rbvh_card_aabb() && hitState.bestPrimitive == DEBUG_HIT_RBVH_CARD_AABB {
        write_hit_output(rayId, DEBUG_HIT_INSTANCE_MARKER, DEBUG_HIT_RBVH_CARD_AABB, 0.0, vec2<f32>(0.0), INVALID_INDEX, 0u);
    } else {
        write_hit_output(rayId, INVALID_INDEX, INVALID_INDEX, 0.0, vec2<f32>(0.0), INVALID_INDEX, 0u);
    }
}

fn intersect_aabb_interval_invdir(rayOrigin: vec3<f32>, invdir: vec3<f32>, minPoint: vec3<f32>, maxPoint: vec3<f32>) -> vec2<f32> {
    let t0 = (minPoint - rayOrigin) * invdir;
    let t1 = (maxPoint - rayOrigin) * invdir;
    let tsmaller = min(t0, t1);
    let tbigger = max(t0, t1);
    let enterT = max(max(tsmaller.x, tsmaller.y), tsmaller.z);
    let exitT = min(min(tbigger.x, tbigger.y), tbigger.z);
    if enterT > exitT {
        return vec2<f32>(1.0, -1.0);
    }
    return vec2<f32>(enterT, exitT);
}

fn stream_task_capacity() -> u32 {
    return max(uniforms.traceFlags.z, 1u);
}

fn stream_hash_slot_capacity() -> u32 {
    return max(stream_task_capacity() * STREAM_GLOBAL_HASH_SLOT_MULTIPLIER, 1u);
}

fn stream_hash_block_capacity() -> u32 {
    return max(stream_task_capacity() * STREAM_GLOBAL_HASH_BLOCK_MULTIPLIER, 1u);
}

fn stream_local_ray_capacity() -> u32 {
    return 1u;
}

fn stream_hash_generation() -> u32 {
    return max(uniforms.traceFlags.w, 1u);
}

fn stream_max_local_node_iterations() -> u32 {
    return clamp((uniforms.traceFlags.y >> 8u) & 0xffu, 1u, STREAM_MAX_LOCAL_NODE_ITERATIONS);
}

fn stream_current_hit_t(rayId: u32) -> f32 {
    if rayId >= uniforms.rayCount {
        return uniforms.tMax;
    }
    return bitcast<f32>(atomicLoad(&streamHitT[rayId]));
}

fn stream_mark_overflow() {
    atomicAdd(&streamCounters.overflowCount, 1u);
    trace_stat_add_local_stack_spill(1u);
}

fn stream_clear_local_buckets(localIndex: u32) {
    for (var bucket = localIndex; bucket < STREAM_LOCAL_BUCKET_COUNT; bucket += STREAM_PACKETS_PER_WORKGROUP * STREAM_PACKET_RAY_CAPACITY) {
        atomicStore(&streamLocalBucketCounts[bucket], 0u);
        streamLocalBucketKinds[bucket] = 0u;
        streamLocalBucketBaseIndices[bucket] = 0u;
        streamLocalBucketMasks[bucket] = 0u;
        streamLocalBucketInstanceIndices[bucket] = INVALID_INDEX;
        streamLocalBucketBlasHandles[bucket] = INVALID_INDEX;
    }
}

fn stream_reset_workgroup_queue(localIndex: u32) {
    if localIndex == 0u {
        streamWorkgroupQueueCount = 0u;
        streamWorkgroupQueueReadCursor = 0u;
        streamWorkgroupQueueBatchStart = 0u;
        streamWorkgroupQueueBatchCount = 0u;
        streamOutputToB = 0u;
    }
}

fn stream_prepare_workgroup_queue_batch() {
    let queueCount = min(streamWorkgroupQueueCount, STREAM_WORKGROUP_QUEUE_CAPACITY);
    let batchStart = min(streamWorkgroupQueueReadCursor, queueCount);
    let batchEnd = min(batchStart + STREAM_PACKETS_PER_WORKGROUP, queueCount);
    streamWorkgroupQueueReadCursor = batchEnd;
    streamWorkgroupQueueBatchStart = batchStart;
    streamWorkgroupQueueBatchCount = batchEnd - batchStart;
}

fn stream_process_task_lane(
    taskKind: u32,
    taskBaseIndex: u32,
    taskMask: u32,
    taskInstanceIndex: u32,
    taskBlasHandle: u32,
    rayRef: u32,
    bucketBase: u32) {
    if taskKind == STREAM_TASK_KIND_TLAS_GROUP {
        if rayRef < uniforms.rayCount {
            let ray = rayInput[rayRef];
            if trace_ray_is_valid(ray) && uniforms.tlasNodeCount != 0u {
                stream_process_tlas_task(taskBaseIndex, taskMask, rayRef, ray, bucketBase);
            }
        }
    } else if taskKind == STREAM_TASK_KIND_BLAS_GROUP {
        stream_process_blas_task(taskBaseIndex, taskMask, taskInstanceIndex, taskBlasHandle, rayRef, bucketBase);
    }
}

fn stream_process_workgroup_queue_batch(localIndex: u32) {
    let packetInWorkgroup = localIndex / STREAM_PACKET_RAY_CAPACITY;
    if packetInWorkgroup >= streamWorkgroupQueueBatchCount {
        return;
    }
    let lane = localIndex % STREAM_PACKET_RAY_CAPACITY;
    let slot = streamWorkgroupQueueBatchStart + packetInWorkgroup;
    let rayCount = streamWorkgroupQueueRayCounts[slot];
    if lane == 0u {
        trace_stat_add_workgroup_queue_pop();
    }
    if lane >= rayCount {
        return;
    }
    stream_process_task_lane(
        streamWorkgroupQueueKinds[slot],
        streamWorkgroupQueueBaseIndices[slot],
        streamWorkgroupQueueMasks[slot],
        streamWorkgroupQueueInstanceIndices[slot],
        streamWorkgroupQueueBlasHandles[slot],
        streamWorkgroupQueueRayRefs[slot * STREAM_PACKET_RAY_CAPACITY + lane],
        packetInWorkgroup * STREAM_BUCKETS_PER_PACKET);
}

fn stream_bucket_matches(bucket: u32, kind: u32, baseIndex: u32, mask: u32, instanceIndex: u32, blasHandle: u32) -> bool {
    return streamLocalBucketKinds[bucket] == kind &&
        streamLocalBucketBaseIndices[bucket] == baseIndex &&
        streamLocalBucketMasks[bucket] == mask &&
        streamLocalBucketInstanceIndices[bucket] == instanceIndex &&
        streamLocalBucketBlasHandles[bucket] == blasHandle;
}

fn stream_spill_single_child(kind: u32, baseIndex: u32, mask: u32, instanceIndex: u32, blasHandle: u32, rayRef: u32) {
    var refs: array<u32, 32>;
    refs[0u] = rayRef;
    for (var i = 1u; i < STREAM_PACKET_RAY_CAPACITY; i += 1u) {
        refs[i] = INVALID_INDEX;
    }

    if streamOutputToB != 0u {
        stream_emit_task_b_refs(kind, baseIndex, mask, instanceIndex, blasHandle, 1u, &refs);
    } else {
        stream_emit_task_a_refs(kind, baseIndex, mask, instanceIndex, blasHandle, 1u, &refs);
    }
    trace_stat_add_workgroup_queue_push();
}

fn stream_append_child(bucketBase: u32, kind: u32, baseIndex: u32, mask: u32, instanceIndex: u32, blasHandle: u32, rayRef: u32) {
    if bucketBase >= STREAM_LOCAL_BUCKET_SENTINEL {
        stream_spill_single_child(kind, baseIndex, mask, instanceIndex, blasHandle, rayRef);
        return;
    }
    let bucketEnd = min(bucketBase + STREAM_BUCKETS_PER_PACKET, STREAM_LOCAL_BUCKET_SENTINEL);
    for (var probe = bucketBase; probe < bucketEnd; probe += 1u) {
        let count = atomicLoad(&streamLocalBucketCounts[probe]);
        if count == STREAM_BUCKET_CLAIMING {
            continue;
        }
        if count != 0u {
            if stream_bucket_matches(probe, kind, baseIndex, mask, instanceIndex, blasHandle) {
                let index = atomicAdd(&streamLocalBucketCounts[probe], 1u);
                if index < STREAM_PACKET_RAY_CAPACITY {
                    streamLocalBucketRayRefs[probe * STREAM_PACKET_RAY_CAPACITY + index] = rayRef;
                    atomicStore(&streamLocalBucketCounts[STREAM_LOCAL_BUCKET_SENTINEL], 1u);
                    return;
                } else {
                    stream_spill_single_child(kind, baseIndex, mask, instanceIndex, blasHandle, rayRef);
                    return;
                }
            }
            continue;
        }

        let claim = atomicCompareExchangeWeak(&streamLocalBucketCounts[probe], 0u, STREAM_BUCKET_CLAIMING);
        if claim.exchanged {
            streamLocalBucketKinds[probe] = kind;
            streamLocalBucketBaseIndices[probe] = baseIndex;
            streamLocalBucketMasks[probe] = mask;
            streamLocalBucketInstanceIndices[probe] = instanceIndex;
            streamLocalBucketBlasHandles[probe] = blasHandle;
            streamLocalBucketRayRefs[probe * STREAM_PACKET_RAY_CAPACITY] = rayRef;
            atomicStore(&streamLocalBucketCounts[STREAM_LOCAL_BUCKET_SENTINEL], 1u);
            atomicStore(&streamLocalBucketCounts[probe], 1u);
            return;
        }
    }

    stream_spill_single_child(kind, baseIndex, mask, instanceIndex, blasHandle, rayRef);
}

fn stream_write_task_b_refs(index: u32, kind: u32, baseIndex: u32, mask: u32, instanceIndex: u32, blasHandle: u32, rayCount: u32, refs: ptr<function, array<u32, 32>>) {
    if index >= stream_task_capacity() {
        stream_mark_overflow();
        return;
    }
    streamTasksB[index].kind = kind;
    streamTasksB[index].baseIndex = baseIndex;
    streamTasksB[index].mask = mask;
    streamTasksB[index].instanceIndex = instanceIndex;
    streamTasksB[index].blasHandle = blasHandle;
    streamTasksB[index].rayCount = rayCount;
    streamTasksB[index].flags = 0u;
    streamTasksB[index]._padding0 = 0u;
    for (var i = 0u; i < STREAM_PACKET_RAY_CAPACITY; i += 1u) {
        streamTasksB[index].rayRefs[i] = select(INVALID_INDEX, (*refs)[i], i < rayCount);
    }
}

fn stream_write_task_a_refs(index: u32, kind: u32, baseIndex: u32, mask: u32, instanceIndex: u32, blasHandle: u32, rayCount: u32, refs: ptr<function, array<u32, 32>>) {
    if index >= stream_task_capacity() {
        stream_mark_overflow();
        return;
    }
    streamTasksA[index].kind = kind;
    streamTasksA[index].baseIndex = baseIndex;
    streamTasksA[index].mask = mask;
    streamTasksA[index].instanceIndex = instanceIndex;
    streamTasksA[index].blasHandle = blasHandle;
    streamTasksA[index].rayCount = rayCount;
    streamTasksA[index].flags = 0u;
    streamTasksA[index]._padding0 = 0u;
    for (var i = 0u; i < STREAM_PACKET_RAY_CAPACITY; i += 1u) {
        streamTasksA[index].rayRefs[i] = select(INVALID_INDEX, (*refs)[i], i < rayCount);
    }
}

fn stream_emit_task_b_refs(kind: u32, baseIndex: u32, mask: u32, instanceIndex: u32, blasHandle: u32, rayCount: u32, refs: ptr<function, array<u32, 32>>) {
    let outIndex = atomicAdd(&streamCounters.taskCountB, 1u);
    stream_write_task_b_refs(outIndex, kind, baseIndex, mask, instanceIndex, blasHandle, rayCount, refs);
}

fn stream_emit_task_a_refs(kind: u32, baseIndex: u32, mask: u32, instanceIndex: u32, blasHandle: u32, rayCount: u32, refs: ptr<function, array<u32, 32>>) {
    let outIndex = atomicAdd(&streamCounters.taskCountA, 1u);
    stream_write_task_a_refs(outIndex, kind, baseIndex, mask, instanceIndex, blasHandle, rayCount, refs);
}

fn stream_write_workgroup_queue_refs(slot: u32, kind: u32, baseIndex: u32, mask: u32, instanceIndex: u32, blasHandle: u32, rayCount: u32, refs: ptr<function, array<u32, 32>>) {
    streamWorkgroupQueueKinds[slot] = kind;
    streamWorkgroupQueueBaseIndices[slot] = baseIndex;
    streamWorkgroupQueueMasks[slot] = mask;
    streamWorkgroupQueueInstanceIndices[slot] = instanceIndex;
    streamWorkgroupQueueBlasHandles[slot] = blasHandle;
    streamWorkgroupQueueRayCounts[slot] = rayCount;
    for (var i = 0u; i < STREAM_PACKET_RAY_CAPACITY; i += 1u) {
        streamWorkgroupQueueRayRefs[slot * STREAM_PACKET_RAY_CAPACITY + i] = select(INVALID_INDEX, (*refs)[i], i < rayCount);
    }
}

fn stream_mix_hash(value: u32) -> u32 {
    var x = value;
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

fn stream_task_key_hash(kind: u32, baseIndex: u32, mask: u32, instanceIndex: u32, blasHandle: u32) -> u32 {
    var h = 0x9e3779b9u;
    h = stream_mix_hash(h ^ (kind + 0x85ebca6bu));
    h = stream_mix_hash(h ^ (baseIndex + 0xc2b2ae35u));
    h = stream_mix_hash(h ^ (mask + 0x27d4eb2fu));
    h = stream_mix_hash(h ^ (instanceIndex + 0x165667b1u));
    h = stream_mix_hash(h ^ (blasHandle + 0xd3a2646cu));
    if h == STREAM_GLOBAL_HASH_EMPTY || h == STREAM_GLOBAL_HASH_CLAIMING {
        h ^= 0x6d2b79f5u;
    }
    if h == STREAM_GLOBAL_HASH_EMPTY || h == STREAM_GLOBAL_HASH_CLAIMING {
        h = 1u;
    }
    return h;
}

fn stream_hash_slot_index(keyHash: u32, shardId: u32, probe: u32) -> u32 {
    let capacity = stream_hash_slot_capacity();
    let shardCount = min(STREAM_GLOBAL_HASH_SHARD_COUNT, capacity);
    let slotsPerShard = max(capacity / shardCount, 1u);
    let shardBase = (shardId % shardCount) * slotsPerShard;
    let mixedProbe = stream_mix_hash(keyHash + probe * 0x9e3779b9u);
    return min(shardBase + (mixedProbe % slotsPerShard), capacity - 1u);
}

fn stream_hash_slot_matches(slot: u32, kind: u32, baseIndex: u32, mask: u32, instanceIndex: u32, blasHandle: u32) -> bool {
    return streamHashSlots[slot].kind == kind &&
        streamHashSlots[slot].baseIndex == baseIndex &&
        streamHashSlots[slot].mask == mask &&
        streamHashSlots[slot].instanceIndex == instanceIndex &&
        streamHashSlots[slot].blasHandle == blasHandle;
}

fn stream_write_dense_overflow_b(kind: u32, baseIndex: u32, mask: u32, instanceIndex: u32, blasHandle: u32, rayCount: u32, refs: ptr<function, array<u32, 32>>) {
    atomicAdd(&streamCounters.hashInsertFailureCount, 1u);
    trace_stat_add_stream_hash_insert_failure();
    let outIndex = atomicAdd(&streamCounters.taskCountB, 1u);
    stream_write_task_b_refs(outIndex, kind, baseIndex, mask, instanceIndex, blasHandle, rayCount, refs);
}

fn stream_write_dense_overflow_a(kind: u32, baseIndex: u32, mask: u32, instanceIndex: u32, blasHandle: u32, rayCount: u32, refs: ptr<function, array<u32, 32>>) {
    atomicAdd(&streamCounters.hashInsertFailureCount, 1u);
    trace_stat_add_stream_hash_insert_failure();
    let outIndex = atomicAdd(&streamCounters.taskCountA, 1u);
    stream_write_task_a_refs(outIndex, kind, baseIndex, mask, instanceIndex, blasHandle, rayCount, refs);
}

fn stream_append_hash_block(slot: u32, rayCount: u32, refs: ptr<function, array<u32, 32>>) -> bool {
    let blockIndex = atomicAdd(&streamCounters.hashBlockCount, 1u);
    if blockIndex >= stream_hash_block_capacity() {
        stream_mark_overflow();
        return false;
    }
    atomicStore(&streamPacketBlocks[blockIndex].nextBlock, INVALID_INDEX);
    streamPacketBlocks[blockIndex].rayCount = rayCount;
    for (var i = 0u; i < STREAM_PACKET_RAY_CAPACITY; i += 1u) {
        streamPacketBlocks[blockIndex].rayRefs[i] = select(INVALID_INDEX, (*refs)[i], i < rayCount);
    }

    let oldTail = atomicExchange(&streamHashSlots[slot].tailBlock, blockIndex);
    if oldTail != INVALID_INDEX {
        atomicStore(&streamPacketBlocks[oldTail].nextBlock, blockIndex);
    } else {
        atomicStore(&streamHashSlots[slot].headBlock, blockIndex);
    }
    return true;
}

fn stream_insert_hash_packet_b(kind: u32, baseIndex: u32, mask: u32, instanceIndex: u32, blasHandle: u32, rayCount: u32, refs: ptr<function, array<u32, 32>>, shardId: u32) {
    let keyHash = stream_task_key_hash(kind, baseIndex, mask, instanceIndex, blasHandle);
    let generation = stream_hash_generation();
    for (var probe = 0u; probe < STREAM_GLOBAL_HASH_MAX_PROBES; probe += 1u) {
        let slot = stream_hash_slot_index(keyHash, shardId, probe);
        let slotGeneration = atomicLoad(&streamHashSlots[slot].generation);
        if slotGeneration == STREAM_GLOBAL_HASH_CLAIMING {
            continue;
        }
        let currentHash = atomicLoad(&streamHashSlots[slot].keyHash);
        if slotGeneration == generation && currentHash == keyHash && stream_hash_slot_matches(slot, kind, baseIndex, mask, instanceIndex, blasHandle) {
            if !stream_append_hash_block(slot, rayCount, refs) {
                stream_write_dense_overflow_b(kind, baseIndex, mask, instanceIndex, blasHandle, rayCount, refs);
            }
            return;
        }
        if slotGeneration != generation {
            let claim = atomicCompareExchangeWeak(&streamHashSlots[slot].generation, slotGeneration, STREAM_GLOBAL_HASH_CLAIMING);
            if claim.exchanged {
                atomicStore(&streamHashSlots[slot].keyHash, STREAM_GLOBAL_HASH_CLAIMING);
                streamHashSlots[slot].kind = kind;
                streamHashSlots[slot].baseIndex = baseIndex;
                streamHashSlots[slot].mask = mask;
                streamHashSlots[slot].instanceIndex = instanceIndex;
                streamHashSlots[slot].blasHandle = blasHandle;
                atomicStore(&streamHashSlots[slot].headBlock, INVALID_INDEX);
                atomicStore(&streamHashSlots[slot].tailBlock, INVALID_INDEX);
                let occupiedIndex = atomicAdd(&streamCounters.hashOccupiedCount, 1u);
                if occupiedIndex < stream_hash_slot_capacity() {
                    streamHashOccupiedSlots[occupiedIndex] = slot;
                }
                atomicStore(&streamHashSlots[slot].keyHash, keyHash);
                atomicStore(&streamHashSlots[slot].generation, generation);
                if !stream_append_hash_block(slot, rayCount, refs) {
                    stream_write_dense_overflow_b(kind, baseIndex, mask, instanceIndex, blasHandle, rayCount, refs);
                }
                return;
            }
        }
    }
    stream_write_dense_overflow_b(kind, baseIndex, mask, instanceIndex, blasHandle, rayCount, refs);
}

fn stream_insert_hash_packet_a(kind: u32, baseIndex: u32, mask: u32, instanceIndex: u32, blasHandle: u32, rayCount: u32, refs: ptr<function, array<u32, 32>>, shardId: u32) {
    let keyHash = stream_task_key_hash(kind, baseIndex, mask, instanceIndex, blasHandle);
    let generation = stream_hash_generation();
    for (var probe = 0u; probe < STREAM_GLOBAL_HASH_MAX_PROBES; probe += 1u) {
        let slot = stream_hash_slot_index(keyHash, shardId, probe);
        let slotGeneration = atomicLoad(&streamHashSlots[slot].generation);
        if slotGeneration == STREAM_GLOBAL_HASH_CLAIMING {
            continue;
        }
        let currentHash = atomicLoad(&streamHashSlots[slot].keyHash);
        if slotGeneration == generation && currentHash == keyHash && stream_hash_slot_matches(slot, kind, baseIndex, mask, instanceIndex, blasHandle) {
            if !stream_append_hash_block(slot, rayCount, refs) {
                stream_write_dense_overflow_a(kind, baseIndex, mask, instanceIndex, blasHandle, rayCount, refs);
            }
            return;
        }
        if slotGeneration != generation {
            let claim = atomicCompareExchangeWeak(&streamHashSlots[slot].generation, slotGeneration, STREAM_GLOBAL_HASH_CLAIMING);
            if claim.exchanged {
                atomicStore(&streamHashSlots[slot].keyHash, STREAM_GLOBAL_HASH_CLAIMING);
                streamHashSlots[slot].kind = kind;
                streamHashSlots[slot].baseIndex = baseIndex;
                streamHashSlots[slot].mask = mask;
                streamHashSlots[slot].instanceIndex = instanceIndex;
                streamHashSlots[slot].blasHandle = blasHandle;
                atomicStore(&streamHashSlots[slot].headBlock, INVALID_INDEX);
                atomicStore(&streamHashSlots[slot].tailBlock, INVALID_INDEX);
                let occupiedIndex = atomicAdd(&streamCounters.hashOccupiedCount, 1u);
                if occupiedIndex < stream_hash_slot_capacity() {
                    streamHashOccupiedSlots[occupiedIndex] = slot;
                }
                atomicStore(&streamHashSlots[slot].keyHash, keyHash);
                atomicStore(&streamHashSlots[slot].generation, generation);
                if !stream_append_hash_block(slot, rayCount, refs) {
                    stream_write_dense_overflow_a(kind, baseIndex, mask, instanceIndex, blasHandle, rayCount, refs);
                }
                return;
            }
        }
    }
    stream_write_dense_overflow_a(kind, baseIndex, mask, instanceIndex, blasHandle, rayCount, refs);
}

fn stream_enqueue_workgroup_or_b_refs(kind: u32, baseIndex: u32, mask: u32, instanceIndex: u32, blasHandle: u32, rayCount: u32, refs: ptr<function, array<u32, 32>>) {
    let localSlot = streamWorkgroupQueueCount;
    streamWorkgroupQueueCount += 1u;
    if localSlot < STREAM_WORKGROUP_QUEUE_CAPACITY {
        stream_write_workgroup_queue_refs(localSlot, kind, baseIndex, mask, instanceIndex, blasHandle, rayCount, refs);
        trace_stat_add_workgroup_queue_push();
    } else {
        stream_emit_task_b_refs(kind, baseIndex, mask, instanceIndex, blasHandle, rayCount, refs);
        trace_stat_add_workgroup_queue_push();
    }
}

fn stream_enqueue_workgroup_or_a_refs(kind: u32, baseIndex: u32, mask: u32, instanceIndex: u32, blasHandle: u32, rayCount: u32, refs: ptr<function, array<u32, 32>>) {
    let localSlot = streamWorkgroupQueueCount;
    streamWorkgroupQueueCount += 1u;
    if localSlot < STREAM_WORKGROUP_QUEUE_CAPACITY {
        stream_write_workgroup_queue_refs(localSlot, kind, baseIndex, mask, instanceIndex, blasHandle, rayCount, refs);
        trace_stat_add_workgroup_queue_push();
    } else {
        stream_emit_task_a_refs(kind, baseIndex, mask, instanceIndex, blasHandle, rayCount, refs);
        trace_stat_add_workgroup_queue_push();
    }
}

fn stream_flush_local_buckets_to_workgroup_or_b() {
    if atomicLoad(&streamLocalBucketCounts[STREAM_LOCAL_BUCKET_SENTINEL]) == 0u {
        return;
    }
    var refs: array<u32, 32>;
    for (var bucket = 0u; bucket < STREAM_LOCAL_BUCKET_SENTINEL; bucket += 1u) {
        let bucketCount = min(atomicLoad(&streamLocalBucketCounts[bucket]), STREAM_PACKET_RAY_CAPACITY);
        if bucketCount == 0u {
            continue;
        }
        let kind = streamLocalBucketKinds[bucket];
        let baseIndex = streamLocalBucketBaseIndices[bucket];
        let mask = streamLocalBucketMasks[bucket];
        let instanceIndex = streamLocalBucketInstanceIndices[bucket];
        let blasHandle = streamLocalBucketBlasHandles[bucket];
        var alreadyMerged = false;
        for (var previous = 0u; previous < bucket; previous += 1u) {
            if atomicLoad(&streamLocalBucketCounts[previous]) != 0u &&
                stream_bucket_matches(previous, kind, baseIndex, mask, instanceIndex, blasHandle) {
                alreadyMerged = true;
            }
        }
        if alreadyMerged {
            continue;
        }

        var refCount = 0u;
        for (var mergeBucket = bucket; mergeBucket < STREAM_LOCAL_BUCKET_SENTINEL; mergeBucket += 1u) {
            let mergeCount = min(atomicLoad(&streamLocalBucketCounts[mergeBucket]), STREAM_PACKET_RAY_CAPACITY);
            if mergeCount == 0u || !stream_bucket_matches(mergeBucket, kind, baseIndex, mask, instanceIndex, blasHandle) {
                continue;
            }
            for (var refIndex = 0u; refIndex < STREAM_PACKET_RAY_CAPACITY; refIndex += 1u) {
                if refIndex >= mergeCount {
                    break;
                }
                refs[refCount] = streamLocalBucketRayRefs[mergeBucket * STREAM_PACKET_RAY_CAPACITY + refIndex];
                refCount += 1u;
                if refCount == STREAM_PACKET_RAY_CAPACITY {
                    stream_enqueue_workgroup_or_b_refs(kind, baseIndex, mask, instanceIndex, blasHandle, refCount, &refs);
                    refCount = 0u;
                }
            }
        }
        if refCount != 0u {
            stream_enqueue_workgroup_or_b_refs(kind, baseIndex, mask, instanceIndex, blasHandle, refCount, &refs);
        }
    }
}

fn stream_flush_local_buckets_to_workgroup_or_a() {
    if atomicLoad(&streamLocalBucketCounts[STREAM_LOCAL_BUCKET_SENTINEL]) == 0u {
        return;
    }
    var refs: array<u32, 32>;
    for (var bucket = 0u; bucket < STREAM_LOCAL_BUCKET_SENTINEL; bucket += 1u) {
        let bucketCount = min(atomicLoad(&streamLocalBucketCounts[bucket]), STREAM_PACKET_RAY_CAPACITY);
        if bucketCount == 0u {
            continue;
        }
        let kind = streamLocalBucketKinds[bucket];
        let baseIndex = streamLocalBucketBaseIndices[bucket];
        let mask = streamLocalBucketMasks[bucket];
        let instanceIndex = streamLocalBucketInstanceIndices[bucket];
        let blasHandle = streamLocalBucketBlasHandles[bucket];
        var alreadyMerged = false;
        for (var previous = 0u; previous < bucket; previous += 1u) {
            if atomicLoad(&streamLocalBucketCounts[previous]) != 0u &&
                stream_bucket_matches(previous, kind, baseIndex, mask, instanceIndex, blasHandle) {
                alreadyMerged = true;
            }
        }
        if alreadyMerged {
            continue;
        }

        var refCount = 0u;
        for (var mergeBucket = bucket; mergeBucket < STREAM_LOCAL_BUCKET_SENTINEL; mergeBucket += 1u) {
            let mergeCount = min(atomicLoad(&streamLocalBucketCounts[mergeBucket]), STREAM_PACKET_RAY_CAPACITY);
            if mergeCount == 0u || !stream_bucket_matches(mergeBucket, kind, baseIndex, mask, instanceIndex, blasHandle) {
                continue;
            }
            for (var refIndex = 0u; refIndex < STREAM_PACKET_RAY_CAPACITY; refIndex += 1u) {
                if refIndex >= mergeCount {
                    break;
                }
                refs[refCount] = streamLocalBucketRayRefs[mergeBucket * STREAM_PACKET_RAY_CAPACITY + refIndex];
                refCount += 1u;
                if refCount == STREAM_PACKET_RAY_CAPACITY {
                    stream_enqueue_workgroup_or_a_refs(kind, baseIndex, mask, instanceIndex, blasHandle, refCount, &refs);
                    refCount = 0u;
                }
            }
        }
        if refCount != 0u {
            stream_enqueue_workgroup_or_a_refs(kind, baseIndex, mask, instanceIndex, blasHandle, refCount, &refs);
        }
    }
}

fn stream_flush_remaining_workgroup_queue_to_b() {
    var refs: array<u32, 32>;
    let queueCount = min(streamWorkgroupQueueCount, STREAM_WORKGROUP_QUEUE_CAPACITY);
    var slot = min(streamWorkgroupQueueReadCursor, queueCount);
    while slot < queueCount {
        let rayCount = min(streamWorkgroupQueueRayCounts[slot], STREAM_PACKET_RAY_CAPACITY);
        for (var i = 0u; i < STREAM_PACKET_RAY_CAPACITY; i += 1u) {
            refs[i] = streamWorkgroupQueueRayRefs[slot * STREAM_PACKET_RAY_CAPACITY + i];
        }
        stream_emit_task_b_refs(
            streamWorkgroupQueueKinds[slot],
            streamWorkgroupQueueBaseIndices[slot],
            streamWorkgroupQueueMasks[slot],
            streamWorkgroupQueueInstanceIndices[slot],
            streamWorkgroupQueueBlasHandles[slot],
            rayCount,
            &refs);
        trace_stat_add_workgroup_queue_push();
        slot += 1u;
    }
}

fn stream_flush_remaining_workgroup_queue_to_a() {
    var refs: array<u32, 32>;
    let queueCount = min(streamWorkgroupQueueCount, STREAM_WORKGROUP_QUEUE_CAPACITY);
    var slot = min(streamWorkgroupQueueReadCursor, queueCount);
    while slot < queueCount {
        let rayCount = min(streamWorkgroupQueueRayCounts[slot], STREAM_PACKET_RAY_CAPACITY);
        for (var i = 0u; i < STREAM_PACKET_RAY_CAPACITY; i += 1u) {
            refs[i] = streamWorkgroupQueueRayRefs[slot * STREAM_PACKET_RAY_CAPACITY + i];
        }
        stream_emit_task_a_refs(
            streamWorkgroupQueueKinds[slot],
            streamWorkgroupQueueBaseIndices[slot],
            streamWorkgroupQueueMasks[slot],
            streamWorkgroupQueueInstanceIndices[slot],
            streamWorkgroupQueueBlasHandles[slot],
            rayCount,
            &refs);
        trace_stat_add_workgroup_queue_push();
        slot += 1u;
    }
}

fn stream_flush_local_buckets_to_b() {
    var refs: array<u32, 32>;
    for (var bucket = 0u; bucket < STREAM_LOCAL_BUCKET_SENTINEL; bucket += 1u) {
        let bucketCount = min(atomicLoad(&streamLocalBucketCounts[bucket]), STREAM_PACKET_RAY_CAPACITY);
        if bucketCount == 0u {
            continue;
        }
        let kind = streamLocalBucketKinds[bucket];
        let baseIndex = streamLocalBucketBaseIndices[bucket];
        let mask = streamLocalBucketMasks[bucket];
        let instanceIndex = streamLocalBucketInstanceIndices[bucket];
        let blasHandle = streamLocalBucketBlasHandles[bucket];
        var alreadyMerged = false;
        for (var previous = 0u; previous < bucket; previous += 1u) {
            if atomicLoad(&streamLocalBucketCounts[previous]) != 0u &&
                stream_bucket_matches(previous, kind, baseIndex, mask, instanceIndex, blasHandle) {
                alreadyMerged = true;
            }
        }
        if alreadyMerged {
            continue;
        }

        var refCount = 0u;
        for (var mergeBucket = bucket; mergeBucket < STREAM_LOCAL_BUCKET_SENTINEL; mergeBucket += 1u) {
            let mergeCount = min(atomicLoad(&streamLocalBucketCounts[mergeBucket]), STREAM_PACKET_RAY_CAPACITY);
            if mergeCount == 0u || !stream_bucket_matches(mergeBucket, kind, baseIndex, mask, instanceIndex, blasHandle) {
                continue;
            }
            for (var refIndex = 0u; refIndex < STREAM_PACKET_RAY_CAPACITY; refIndex += 1u) {
                if refIndex >= mergeCount {
                    break;
                }
                refs[refCount] = streamLocalBucketRayRefs[mergeBucket * STREAM_PACKET_RAY_CAPACITY + refIndex];
                refCount += 1u;
                if refCount == STREAM_PACKET_RAY_CAPACITY {
                    let outIndex = atomicAdd(&streamCounters.taskCountB, 1u);
                    stream_write_task_b_refs(outIndex, kind, baseIndex, mask, instanceIndex, blasHandle, refCount, &refs);
                    trace_stat_add_workgroup_queue_push();
                    refCount = 0u;
                }
            }
        }
        if refCount != 0u {
            let outIndex = atomicAdd(&streamCounters.taskCountB, 1u);
            stream_write_task_b_refs(outIndex, kind, baseIndex, mask, instanceIndex, blasHandle, refCount, &refs);
            trace_stat_add_workgroup_queue_push();
        }
    }
}

fn stream_flush_local_buckets_to_a() {
    var refs: array<u32, 32>;
    for (var bucket = 0u; bucket < STREAM_LOCAL_BUCKET_SENTINEL; bucket += 1u) {
        let bucketCount = min(atomicLoad(&streamLocalBucketCounts[bucket]), STREAM_PACKET_RAY_CAPACITY);
        if bucketCount == 0u {
            continue;
        }
        let kind = streamLocalBucketKinds[bucket];
        let baseIndex = streamLocalBucketBaseIndices[bucket];
        let mask = streamLocalBucketMasks[bucket];
        let instanceIndex = streamLocalBucketInstanceIndices[bucket];
        let blasHandle = streamLocalBucketBlasHandles[bucket];
        var alreadyMerged = false;
        for (var previous = 0u; previous < bucket; previous += 1u) {
            if atomicLoad(&streamLocalBucketCounts[previous]) != 0u &&
                stream_bucket_matches(previous, kind, baseIndex, mask, instanceIndex, blasHandle) {
                alreadyMerged = true;
            }
        }
        if alreadyMerged {
            continue;
        }

        var refCount = 0u;
        for (var mergeBucket = bucket; mergeBucket < STREAM_LOCAL_BUCKET_SENTINEL; mergeBucket += 1u) {
            let mergeCount = min(atomicLoad(&streamLocalBucketCounts[mergeBucket]), STREAM_PACKET_RAY_CAPACITY);
            if mergeCount == 0u || !stream_bucket_matches(mergeBucket, kind, baseIndex, mask, instanceIndex, blasHandle) {
                continue;
            }
            for (var refIndex = 0u; refIndex < STREAM_PACKET_RAY_CAPACITY; refIndex += 1u) {
                if refIndex >= mergeCount {
                    break;
                }
                refs[refCount] = streamLocalBucketRayRefs[mergeBucket * STREAM_PACKET_RAY_CAPACITY + refIndex];
                refCount += 1u;
                if refCount == STREAM_PACKET_RAY_CAPACITY {
                    let outIndex = atomicAdd(&streamCounters.taskCountA, 1u);
                    stream_write_task_a_refs(outIndex, kind, baseIndex, mask, instanceIndex, blasHandle, refCount, &refs);
                    trace_stat_add_workgroup_queue_push();
                    refCount = 0u;
                }
            }
        }
        if refCount != 0u {
            let outIndex = atomicAdd(&streamCounters.taskCountA, 1u);
            stream_write_task_a_refs(outIndex, kind, baseIndex, mask, instanceIndex, blasHandle, refCount, &refs);
            trace_stat_add_workgroup_queue_push();
        }
    }
}

fn stream_commit_hit(rayId: u32, hitState: HitState) {
    if rayId >= uniforms.rayCount || hitState.bestInstance == INVALID_INDEX {
        return;
    }
    let hitBits = bitcast<u32>(hitState.bestT);
    let oldBits = atomicMin(&streamHitT[rayId], hitBits);
    let previousT = bitcast<f32>(oldBits);
    let previousHit = hitOutput[rayId];
    let tieBreaksPrevious = abs(hitState.bestT - previousT) <= hit_tie_epsilon(hitState.bestT, previousT) &&
        (previousHit.triangleId == INVALID_INDEX || hitState.bestPrimitive < previousHit.triangleId);
    if hitBits < oldBits || tieBreaksPrevious {
        let sceneInstanceId = tlasInstances[hitState.bestInstance].sceneInstanceId;
        write_hit_output(rayId, sceneInstanceId, hitState.bestPrimitive, hitState.bestT, hitState.bestBary, hitState.bestInstance, 1u);
    }
}

fn tlas_dobb_enabled() -> bool {
    return uniforms.twoStageControl.w != 0u;
}

fn tlas_dobb_node_active(nodeIndex: u32) -> bool {
    if !tlas_dobb_enabled() {
        return false;
    }
    return (tlasNodeMetadata[nodeIndex].flags & TLAS_DOBB_NODE_FLAG_ORIENTED) != 0u;
}

fn blas_dobb_enabled() -> bool {
    return (uniforms.traceFlags.y & 1u) != 0u;
}

fn blas_dobb_node_active(globalNodeIndex: u32) -> bool {
    if !blas_dobb_enabled() {
        return false;
    }
    return (blasNodeMetadata[globalNodeIndex].flags & TLAS_DOBB_NODE_FLAG_ORIENTED) != 0u;
}

fn tlas_dobb_rotate_vector(value: vec3<f32>, rotationIndex: u32) -> vec3<f32> {
    let matrixIndex = rotationIndex % TLAS_DOBB_ROTATION_COUNT;
    return TLAS_DOBB_ROTATION_TABLE[matrixIndex] * value;
}

fn stream_emit_cwbvh_node_children(
    bucketBase: u32,
    n0: vec4<u32>,
    n1: vec4<u32>,
    n2: vec4<u32>,
    n3: vec4<u32>,
    n4: vec4<u32>,
    rayOrigin: vec3<f32>,
    rayDirection: vec3<f32>,
    parentOctinv: u32,
    tmin: f32,
    tmax: f32,
    childKind: u32,
    instanceIndex: u32,
    blasHandle: u32,
    useSlotOrderForInternalChildren: bool,
    rayRef: u32) {
    let invdir = safe_inverse_direction(rayDirection);
    let p = vec3<f32>(bitcast<f32>(n0.x), bitcast<f32>(n0.y), bitcast<f32>(n0.z));
    let e_imask = unpack4xU8(n0.w);
    let octinv4 = parentOctinv * 0x01010101u;
    let adjusted_idirx: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.x) + 127) << 23u) * invdir.x;
    let adjusted_idiry: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.y) + 127) << 23u) * invdir.y;
    let adjusted_idirz: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.z) + 127) << 23u) * invdir.z;
    let origx: f32 = -(rayOrigin.x - p.x) * invdir.x;
    let origy: f32 = -(rayOrigin.y - p.y) * invdir.y;
    let origz: f32 = -(rayOrigin.z - p.z) * invdir.z;
    var hitmask = 0u;
    var childHitBits0 = vec4<u32>(0u);
    var childHitBits1 = vec4<u32>(0u);

    {
        let meta4: u32 = n1.z;
        let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
        let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
        let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
        let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
        if child_bits4 != 0u {
            let swizzledLox = select(n2.x, n3.z, invdir.x < 0.0);
            let swizzledHix = select(n3.z, n2.x, invdir.x < 0.0);
            let swizzledLoy = select(n2.z, n4.x, invdir.y < 0.0);
            let swizzledHiy = select(n4.x, n2.z, invdir.y < 0.0);
            let swizzledLoz = select(n3.x, n4.z, invdir.z < 0.0);
            let swizzledHiz = select(n4.z, n3.x, invdir.z < 0.0);
            for (var i = 0u; i < 4u; i += 1u) {
                let childBits = extract_byte(child_bits4, i);
                if childBits != 0u {
                    let shift = i * 8u;
                    let cmin = max(max(max(
                        f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                        f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                        f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                        tmin);
                    let cmax = min(min(min(
                        f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                        f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                        f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                        tmax);
                    if cmin <= cmax {
                        let bitIndex = extract_byte(bit_index4, i);
                        let hitBits = childBits << bitIndex;
                        childHitBits0[i] = hitBits;
                        hitmask |= hitBits;
                    }
                }
            }
        }
    }
    {
        let meta4: u32 = n1.w;
        let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
        let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
        let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
        let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
        if child_bits4 != 0u {
            let swizzledLox = select(n2.y, n3.w, invdir.x < 0.0);
            let swizzledHix = select(n3.w, n2.y, invdir.x < 0.0);
            let swizzledLoy = select(n2.w, n4.y, invdir.y < 0.0);
            let swizzledHiy = select(n4.y, n2.w, invdir.y < 0.0);
            let swizzledLoz = select(n3.y, n4.w, invdir.z < 0.0);
            let swizzledHiz = select(n4.w, n3.y, invdir.z < 0.0);
            for (var i = 0u; i < 4u; i += 1u) {
                let childBits = extract_byte(child_bits4, i);
                if childBits != 0u {
                    let shift = i * 8u;
                    let cmin = max(max(max(
                        f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                        f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                        f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                        tmin);
                    let cmax = min(min(min(
                        f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                        f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                        f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                        tmax);
                    if cmin <= cmax {
                        let bitIndex = extract_byte(bit_index4, i);
                        let hitBits = childBits << bitIndex;
                        childHitBits1[i] = hitBits;
                        hitmask |= hitBits;
                    }
                }
            }
        }
    }

    let internalGroupMask = (hitmask & 0xFF000000u) | e_imask.w;
    let internalPrefixMask = internalGroupMask & 0x00FFFFFFu;
    for (var childSlot = 0u; childSlot < STREAM_CHILD_PACKET_CAPACITY; childSlot += 1u) {
        var hitBits = 0u;
        if childSlot < 4u {
            hitBits = childHitBits0[childSlot];
        } else {
            hitBits = childHitBits1[childSlot - 4u];
        }
        if hitBits == 0u {
            continue;
        }
        let childIsInternal = (hitBits & 0xFF000000u) != 0u;
        let baseIndex = select(n1.y, n1.x, childIsInternal);
        let scheduledMaskBase = select(hitBits, hitBits | internalPrefixMask, childIsInternal);
        let scheduledMask = scheduledMaskBase | select(0u, TLAS_GROUP_SLOT_ORDER_MASK_FLAG, useSlotOrderForInternalChildren && childIsInternal);
        stream_append_child(bucketBase, childKind, baseIndex, scheduledMask, instanceIndex, blasHandle, rayRef);
    }
}

fn stream_process_tlas_leaf_group(taskBaseIndex: u32, taskMask: u32, rayId: u32, ray: TraceRay, bucketBase: u32) {
    let rayOrigin = trace_ray_origin(ray);
    let rayDirection = trace_ray_direction(ray);
    let invdir = safe_inverse_direction(rayDirection);
    let bestT = stream_current_hit_t(rayId);
    var outputSlot = 0u;
    var mask = taskMask & 0x00FFFFFFu;
    while mask != 0u && outputSlot < STREAM_BUCKETS_PER_PACKET {
        let leafBit = most_significant_bit_index(mask);
        let instanceIndex = tlasLeafInstances[taskBaseIndex + leafBit];
        if instanceIndex != INVALID_INDEX {
            let leaf = tlasInstanceAabbs[instanceIndex];
            let worldInterval = intersect_aabb_interval_invdir(rayOrigin, invdir, leaf.minPoint, leaf.maxPoint);
            let nearT = max(worldInterval.x, uniforms.tMin);
            let farT = min(worldInterval.y, min(bestT, uniforms.tMax));
            if leaf.blasHandle != INVALID_INDEX && nearT <= farT {
                let instance = tlasInstances[leaf.instanceIndex];
                let inverseTransform = instance.inverseTransform;
                let localOrigin = (inverseTransform * vec4<f32>(rayOrigin, 1.0)).xyz;
                let localDirection = (inverseTransform * vec4<f32>(rayDirection, 0.0)).xyz;
                if !any(localOrigin != localOrigin) && !any(localDirection != localDirection) && dot(localDirection, localDirection) > 1.0e-12 {
                    stream_append_child(bucketBase, STREAM_TASK_KIND_BLAS_GROUP, 0u, 0x80000000u, leaf.instanceIndex, leaf.blasHandle, rayId);
                }
            }
        }
        mask &= ~(1u << leafBit);
        outputSlot += 1u;
    }
}

fn stream_process_tlas_task(taskBaseIndex: u32, taskMask: u32, rayId: u32, ray: TraceRay, bucketBase: u32) {
    if (taskMask & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) == 0u {
        return;
    }
    if (taskMask & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) <= 0x00FFFFFFu {
        stream_process_tlas_leaf_group(taskBaseIndex, taskMask, rayId, ray, bucketBase);
        return;
    }

    trace_stat_add_tlas_node_visit(1u);
    let rayOrigin = trace_ray_origin(ray);
    let rayDirection = trace_ray_direction(ray);
    let baseMask = taskMask & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG;
    let parentUsesSlotOrder = (taskMask & TLAS_GROUP_SLOT_ORDER_MASK_FLAG) != 0u;
    let octinv = 7u - ((select(0u, 4u, rayDirection.x < 0.0)) | (select(0u, 2u, rayDirection.y < 0.0)) | (select(0u, 1u, rayDirection.z < 0.0)));
    let parentOctinv = select(octinv, 0u, parentUsesSlotOrder);
    let childBitIndex = most_significant_bit_index(baseMask);
    let remainingMask = baseMask & ~(1u << childBitIndex);
    if remainingMask != 0u {
        stream_append_child(bucketBase, STREAM_TASK_KIND_TLAS_GROUP, taskBaseIndex, remainingMask | select(0u, TLAS_GROUP_SLOT_ORDER_MASK_FLAG, parentUsesSlotOrder && remainingMask > 0x00FFFFFFu), INVALID_INDEX, INVALID_INDEX, rayId);
    }

    let slotIndex = (childBitIndex - 24u) ^ parentOctinv;
    let childNodeIndex = taskBaseIndex + countOneBits(baseMask & ~(0xFFFFFFFFu << slotIndex));
    if childNodeIndex >= uniforms.tlasNodeCount {
        return;
    }
    let nodeBase = childNodeIndex * 5u;
    let nodeUsesDobb = tlas_dobb_node_active(childNodeIndex);
    let nodeRotationIndex = select(0u, tlasNodeMetadata[childNodeIndex].rotationIndex, nodeUsesDobb);
    let n0 = select(tlasNodes[nodeBase + 0u], tlasDobbNodes[nodeBase + 0u], nodeUsesDobb);
    let n1 = select(tlasNodes[nodeBase + 1u], tlasDobbNodes[nodeBase + 1u], nodeUsesDobb);
    let n2 = select(tlasNodes[nodeBase + 2u], tlasDobbNodes[nodeBase + 2u], nodeUsesDobb);
    let n3 = select(tlasNodes[nodeBase + 3u], tlasDobbNodes[nodeBase + 3u], nodeUsesDobb);
    let n4 = select(tlasNodes[nodeBase + 4u], tlasDobbNodes[nodeBase + 4u], nodeUsesDobb);
    let nodeRayOrigin = select(rayOrigin, tlas_dobb_rotate_vector(rayOrigin, nodeRotationIndex), nodeUsesDobb);
    let nodeRayDirection = select(rayDirection, tlas_dobb_rotate_vector(rayDirection, nodeRotationIndex), nodeUsesDobb);
    stream_emit_cwbvh_node_children(
        bucketBase,
        n0,
        n1,
        n2,
        n3,
        n4,
        nodeRayOrigin,
        nodeRayDirection,
        select(parentOctinv, 0u, nodeUsesDobb),
        trace_ray_tmin(ray),
        min(stream_current_hit_t(rayId), trace_ray_tmax(ray)),
        STREAM_TASK_KIND_TLAS_GROUP,
        INVALID_INDEX,
        INVALID_INDEX,
        nodeUsesDobb,
        rayId);
}

fn stream_build_local_blas_ray(rayId: u32, instanceIndex: u32, blasHandle: u32) -> StreamLocalRay {
    var localRay: StreamLocalRay;
    localRay.rayId = INVALID_INDEX;
    localRay.instanceIndex = INVALID_INDEX;
    localRay.blasHandle = INVALID_INDEX;
    localRay._padding0 = 0u;
    localRay.localOriginTMin = vec4<f32>(0.0, 0.0, 0.0, uniforms.tMin);
    localRay.localDirectionTMax = vec4<f32>(0.0, 0.0, -1.0, uniforms.tMax);

    if rayId >= uniforms.rayCount || instanceIndex == INVALID_INDEX || blasHandle == INVALID_INDEX {
        return localRay;
    }
    let ray = rayInput[rayId];
    if !trace_ray_is_valid(ray) {
        return localRay;
    }
    let instance = tlasInstances[instanceIndex];
    let rayOrigin = trace_ray_origin(ray);
    let rayDirection = trace_ray_direction(ray);
    let inverseTransform = instance.inverseTransform;
    let localOrigin = (inverseTransform * vec4<f32>(rayOrigin, 1.0)).xyz;
    let localDirection = (inverseTransform * vec4<f32>(rayDirection, 0.0)).xyz;
    if any(localOrigin != localOrigin) || any(localDirection != localDirection) || dot(localDirection, localDirection) <= 1.0e-12 {
        return localRay;
    }

    localRay.rayId = rayId;
    localRay.instanceIndex = instanceIndex;
    localRay.blasHandle = blasHandle;
    localRay.localOriginTMin = vec4<f32>(localOrigin, trace_ray_tmin(ray));
    localRay.localDirectionTMax = vec4<f32>(localDirection, trace_ray_tmax(ray));
    return localRay;
}

fn stream_process_blas_leaf_group(taskBaseIndex: u32, taskMask: u32, instanceIndex: u32, blasHandle: u32, rayId: u32) {
    let localRay = stream_build_local_blas_ray(rayId, instanceIndex, blasHandle);
    if localRay.rayId >= uniforms.rayCount {
        return;
    }
    if localRay.instanceIndex == INVALID_INDEX || localRay.blasHandle == INVALID_INDEX {
        return;
    }
    let metadata = load_blas_metadata_common(localRay.blasHandle);
    if metadata.handleId == INVALID_INDEX {
        return;
    }
    let primitiveMetadata = load_trace_primitive_metadata(localRay.blasHandle);
    let instance = tlasInstances[localRay.instanceIndex];
    let localOrigin = localRay.localOriginTMin.xyz;
    let localDirection = localRay.localDirectionTMax.xyz;
    let invdir = safe_inverse_direction(localDirection);
    var state = HitState(min(stream_current_hit_t(localRay.rayId), localRay.localDirectionTMax.w), INVALID_INDEX, INVALID_INDEX, vec2<f32>(0.0), 0u);
    var mask = taskMask & 0x00FFFFFFu;
    while mask != 0u {
        let primitiveBit = most_significant_bit_index(mask);
        let localPrimitiveIndex = load_local_leaf_remap(metadata.leafRemapOffset + taskBaseIndex + primitiveBit);
        trace_blas_primitive_leaf_local_cached(
            trace_ray_origin(rayInput[localRay.rayId]),
            trace_ray_direction(rayInput[localRay.rayId]),
            instance,
            localRay.instanceIndex,
            metadata,
            primitiveMetadata,
            localPrimitiveIndex,
            localOrigin,
            localDirection,
            invdir,
            &state);
        mask &= ~(1u << primitiveBit);
    }
    stream_commit_hit(localRay.rayId, state);
}

fn stream_process_blas_task(taskBaseIndex: u32, taskMask: u32, taskInstanceIndex: u32, taskBlasHandle: u32, rayId: u32, bucketBase: u32) {
    if (taskMask & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) == 0u {
        return;
    }
    if (taskMask & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) <= 0x00FFFFFFu {
        stream_process_blas_leaf_group(taskBaseIndex, taskMask, taskInstanceIndex, taskBlasHandle, rayId);
        return;
    }

    let localRay = stream_build_local_blas_ray(rayId, taskInstanceIndex, taskBlasHandle);
    if localRay.rayId >= uniforms.rayCount || localRay.blasHandle == INVALID_INDEX {
        return;
    }
    let metadata = load_blas_metadata_common(localRay.blasHandle);
    if metadata.handleId == INVALID_INDEX || metadata.cwbvhNodeCount == 0u {
        return;
    }
    trace_stat_add_blas_node_visit(1u);
    let localOrigin = localRay.localOriginTMin.xyz;
    let localDirection = localRay.localDirectionTMax.xyz;
    let baseMask = taskMask & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG;
    let parentUsesSlotOrder = (taskMask & TLAS_GROUP_SLOT_ORDER_MASK_FLAG) != 0u;
    var octinv = ((select(0u, 4u, localDirection.x < 0.0)) | (select(0u, 2u, localDirection.y < 0.0)) | (select(0u, 1u, localDirection.z < 0.0)));
    octinv = 7u - octinv;
    let parentOctinv = select(octinv, 0u, parentUsesSlotOrder);
    let childBitIndex = most_significant_bit_index(baseMask);
    let remainingMask = baseMask & ~(1u << childBitIndex);
    if remainingMask != 0u {
        stream_append_child(bucketBase, STREAM_TASK_KIND_BLAS_GROUP, taskBaseIndex, remainingMask | select(0u, TLAS_GROUP_SLOT_ORDER_MASK_FLAG, parentUsesSlotOrder && remainingMask > 0x00FFFFFFu), localRay.instanceIndex, localRay.blasHandle, localRay.rayId);
    }

    let slotIndex = (childBitIndex - 24u) ^ parentOctinv;
    let childNodeIndex = taskBaseIndex + countOneBits(baseMask & ~(0xFFFFFFFFu << slotIndex));
    if childNodeIndex >= metadata.cwbvhNodeCount {
        return;
    }
    let globalNodeIndex = metadata.cwbvhNodeOffset + childNodeIndex;
    let nodeBase = globalNodeIndex * 5u;
    let nodeUsesDobb = blas_dobb_node_active(globalNodeIndex);
    let nodeRotationIndex = select(0u, blasNodeMetadata[globalNodeIndex].rotationIndex, nodeUsesDobb);
    let n0 = select(blasNodes[nodeBase + 0u], blasDobbNodes[nodeBase + 0u], nodeUsesDobb);
    let n1 = select(blasNodes[nodeBase + 1u], blasDobbNodes[nodeBase + 1u], nodeUsesDobb);
    let n2 = select(blasNodes[nodeBase + 2u], blasDobbNodes[nodeBase + 2u], nodeUsesDobb);
    let n3 = select(blasNodes[nodeBase + 3u], blasDobbNodes[nodeBase + 3u], nodeUsesDobb);
    let n4 = select(blasNodes[nodeBase + 4u], blasDobbNodes[nodeBase + 4u], nodeUsesDobb);
    let nodeRayOrigin = select(localOrigin, tlas_dobb_rotate_vector(localOrigin, nodeRotationIndex), nodeUsesDobb);
    let nodeRayDirection = select(localDirection, tlas_dobb_rotate_vector(localDirection, nodeRotationIndex), nodeUsesDobb);
    stream_emit_cwbvh_node_children(
        bucketBase,
        n0,
        n1,
        n2,
        n3,
        n4,
        nodeRayOrigin,
        nodeRayDirection,
        select(parentOctinv, 0u, nodeUsesDobb),
        localRay.localOriginTMin.w,
        min(stream_current_hit_t(localRay.rayId), localRay.localDirectionTMax.w),
        STREAM_TASK_KIND_BLAS_GROUP,
        localRay.instanceIndex,
        localRay.blasHandle,
        nodeUsesDobb,
        localRay.rayId);
}

fn trace_instance_blas_local(rayOrigin: vec3<f32>, rayDirection: vec3<f32>, instanceIndex: u32, entryT: f32, startGroup: vec2<u32>, state: ptr<function, HitState>) {
    if entryT > (*state).bestT {
        return;
    }

    let instance = tlasInstances[instanceIndex];
    let blasHandle = instance.blasHandle;
    if blasHandle == INVALID_INDEX {
        return;
    }

    let metadata = load_blas_metadata_common(blasHandle);
    if metadata.handleId == INVALID_INDEX || metadata.cwbvhNodeCount == 0u {
        return;
    }
    let primitiveMetadata = load_trace_primitive_metadata(blasHandle);

    let inverseTransform = instance.inverseTransform;
    let localOrigin = (inverseTransform * vec4<f32>(rayOrigin, 1.0)).xyz;
    let localDirection = (inverseTransform * vec4<f32>(rayDirection, 0.0)).xyz;
    if any(localOrigin != localOrigin) || any(localDirection != localDirection) || dot(localDirection, localDirection) <= 1.0e-12 {
        return;
    }
    let invdir = safe_inverse_direction(localDirection);
    var octinv = ((select(0u, 4u, localDirection.x < 0.0)) | (select(0u, 2u, localDirection.y < 0.0)) | (select(0u, 1u, localDirection.z < 0.0)));
    octinv = 7u - octinv;

    var traversalStack: array<vec4<u32>, TLAS_TRAVERSAL_STACK_SIZE>;
    var stackPtr: u32 = 0u;
    var ngroup = startGroup;
    var ngroupMinT = 0.0;
    var tgroup = vec2<u32>(0u, 0u);
    var tgroupMinT = uniforms.tMax;
    let orig = localOrigin;
    var tmin = uniforms.tMin;
    var tmax = min((*state).bestT, uniforms.tMax);
    let earlyOut = traversal_early_out_enabled();
    let earlyOutBias = max(uniforms.traceControl.y, 0.0);

    loop {
        if (ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) > 0x00FFFFFFu && should_skip_group_fast(ngroupMinT, (*state).bestT, earlyOut, earlyOutBias) {
            ngroup = vec2<u32>(0u, 0u);
        }

        if (ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) > 0x00FFFFFFu {
            (*state).iterationCount += 1u;
            let parentUsesSlotOrder = (ngroup.y & TLAS_GROUP_SLOT_ORDER_MASK_FLAG) != 0u;
            let imask = ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG;
            let parentOctinv = select(octinv, 0u, parentUsesSlotOrder);
            let childBitIndex = most_significant_bit_index(imask);
            let childNodeBaseIndex = ngroup.x;

            let remainingMask = imask & ~(1u << childBitIndex);
            ngroup.y = remainingMask | select(0u, TLAS_GROUP_SLOT_ORDER_MASK_FLAG, parentUsesSlotOrder && remainingMask > 0x00FFFFFFu);
            if (ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) > 0x00FFFFFFu {
                if !push_tlas_stack_entry(&traversalStack, &stackPtr, ngroup.x, ngroup.y, ngroupMinT) {
                    return;
                }
            }

            let slotIndex: u32 = (childBitIndex - 24u) ^ parentOctinv;
            let childNodeIndex: u32 = childNodeBaseIndex + countOneBits(imask & ~(0xFFFFFFFFu << slotIndex));
            let globalNodeIndex = metadata.cwbvhNodeOffset + childNodeIndex;
            let nodeBase = globalNodeIndex * 5u;
            let nodeUsesDobb = blas_dobb_node_active(globalNodeIndex);
            let nodeRotationIndex = select(0u, blasNodeMetadata[globalNodeIndex].rotationIndex, nodeUsesDobb);
            let n0 = select(blasNodes[nodeBase + 0u], blasDobbNodes[nodeBase + 0u], nodeUsesDobb);
            let n1 = select(blasNodes[nodeBase + 1u], blasDobbNodes[nodeBase + 1u], nodeUsesDobb);
            let n2 = select(blasNodes[nodeBase + 2u], blasDobbNodes[nodeBase + 2u], nodeUsesDobb);
            let n3 = select(blasNodes[nodeBase + 3u], blasDobbNodes[nodeBase + 3u], nodeUsesDobb);
            let n4 = select(blasNodes[nodeBase + 4u], blasDobbNodes[nodeBase + 4u], nodeUsesDobb);
            let nodeRayOrigin = select(localOrigin, tlas_dobb_rotate_vector(localOrigin, nodeRotationIndex), nodeUsesDobb);
            let nodeRayDirection = select(localDirection, tlas_dobb_rotate_vector(localDirection, nodeRotationIndex), nodeUsesDobb);
            let nodeInvdir = safe_inverse_direction(nodeRayDirection);
            let nodeOctinv = select(octinv, 0u, nodeUsesDobb);
            let p = vec3<f32>(bitcast<f32>(n0.x), bitcast<f32>(n0.y), bitcast<f32>(n0.z));
            let e_imask = unpack4xU8(n0.w);
            let octinv4 = nodeOctinv * 0x01010101u;
            let adjusted_idirx: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.x) + 127) << 23u) * nodeInvdir.x;
            let adjusted_idiry: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.y) + 127) << 23u) * nodeInvdir.y;
            let adjusted_idirz: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.z) + 127) << 23u) * nodeInvdir.z;
            let origx: f32 = -(nodeRayOrigin.x - p.x) * nodeInvdir.x;
            let origy: f32 = -(nodeRayOrigin.y - p.y) * nodeInvdir.y;
            let origz: f32 = -(nodeRayOrigin.z - p.z) * nodeInvdir.z;
            var hitmask = 0u;
            var childHitBits0 = vec4<u32>(0u);
            var childHitBits1 = vec4<u32>(0u);
            var childMinT0 = vec4<f32>(uniforms.tMax);
            var childMinT1 = vec4<f32>(uniforms.tMax);

            {
                let meta4: u32 = n1.z;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.x, n3.z, nodeInvdir.x < 0.0);
                    let swizzledHix = select(n3.z, n2.x, nodeInvdir.x < 0.0);
                    let swizzledLoy = select(n2.z, n4.x, nodeInvdir.y < 0.0);
                    let swizzledHiy = select(n4.x, n2.z, nodeInvdir.y < 0.0);
                    let swizzledLoz = select(n3.x, n4.z, nodeInvdir.z < 0.0);
                    let swizzledHiz = select(n4.z, n3.x, nodeInvdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits0[i] = hitBits;
                                childMinT0[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }
            {
                let meta4: u32 = n1.w;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.y, n3.w, nodeInvdir.x < 0.0);
                    let swizzledHix = select(n3.w, n2.y, nodeInvdir.x < 0.0);
                    let swizzledLoy = select(n2.w, n4.y, nodeInvdir.y < 0.0);
                    let swizzledHiy = select(n4.y, n2.w, nodeInvdir.y < 0.0);
                    let swizzledLoz = select(n3.y, n4.w, nodeInvdir.z < 0.0);
                    let swizzledHiz = select(n4.w, n3.y, nodeInvdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits1[i] = hitBits;
                                childMinT1[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }

            var bestBaseIndex = 0u;
            var bestMask = 0u;
            var bestMinT = uniforms.tMax;
            var bestIsInternal = false;
            let internalGroupMask = (hitmask & 0xFF000000u) | e_imask.w;
            let internalPrefixMask = internalGroupMask & 0x00FFFFFFu;
            for (var childSlot = 0u; childSlot < 8u; childSlot += 1u) {
                var hitBits = 0u;
                var childMinT = uniforms.tMax;
                if childSlot < 4u {
                    hitBits = childHitBits0[childSlot];
                    childMinT = childMinT0[childSlot];
                } else {
                    hitBits = childHitBits1[childSlot - 4u];
                    childMinT = childMinT1[childSlot - 4u];
                }
                if hitBits == 0u {
                    continue;
                }
                let childIsInternal = (hitBits & 0xFF000000u) != 0u;
                let baseIndex = select(n1.y, n1.x, childIsInternal);
                let scheduledMaskBase = select(hitBits, hitBits | internalPrefixMask, childIsInternal);
                let scheduledMask = scheduledMaskBase | select(0u, TLAS_GROUP_SLOT_ORDER_MASK_FLAG, nodeUsesDobb && childIsInternal);
                if bestMask == 0u || childMinT < bestMinT {
                    if bestMask != 0u && !push_tlas_stack_entry(&traversalStack, &stackPtr, bestBaseIndex, bestMask, bestMinT) {
                        return;
                    }
                    bestBaseIndex = baseIndex;
                    bestMask = scheduledMask;
                    bestMinT = childMinT;
                    bestIsInternal = childIsInternal;
                } else if !push_tlas_stack_entry(&traversalStack, &stackPtr, baseIndex, scheduledMask, childMinT) {
                    return;
                }
            }
            if bestMask == 0u {
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = uniforms.tMax;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = uniforms.tMax;
            } else if bestIsInternal {
                ngroup = vec2<u32>(bestBaseIndex, bestMask);
                ngroupMinT = bestMinT;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = uniforms.tMax;
            } else {
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = uniforms.tMax;
                tgroup = vec2<u32>(bestBaseIndex, bestMask);
                tgroupMinT = bestMinT;
            }
        } else {
            tgroup = ngroup;
            tgroupMinT = ngroupMinT;
            ngroup = vec2<u32>(0u, 0u);
        }

        if tgroup.y != 0u && should_skip_group_fast(tgroupMinT, (*state).bestT, earlyOut, earlyOutBias) {
            tgroup.y = 0u;
        } else {
            while tgroup.y != 0u {
                let primitiveBit = most_significant_bit_index(tgroup.y);
                let localPrimitiveIndex = load_local_leaf_remap(metadata.leafRemapOffset + tgroup.x + primitiveBit);
                trace_blas_primitive_leaf_local_cached(rayOrigin, rayDirection, instance, instanceIndex, metadata, primitiveMetadata, localPrimitiveIndex, localOrigin, localDirection, invdir, state);
                tgroup.y &= ~(1u << primitiveBit);
            }
            tmax = min((*state).bestT, uniforms.tMax);
        }

        if (ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) <= 0x00FFFFFFu {
            let entry = pop_nearest_stack_entry(&traversalStack, &stackPtr, uniforms.tMax, (*state).bestT, earlyOut, earlyOutBias);
            if entry.y != 0u {
                ngroup = entry.xy;
                ngroupMinT = bitcast<f32>(entry.z);
            } else {
                return;
            }
        }
    }
}

fn trace_instance_blas_any_hit_local(rayOrigin: vec3<f32>, rayDirection: vec3<f32>, instanceIndex: u32, entryT: f32, maxRayT: f32) -> bool {
    if entryT > maxRayT {
        return false;
    }

    let instance = tlasInstances[instanceIndex];
    let blasHandle = instance.blasHandle;
    if blasHandle == INVALID_INDEX {
        return false;
    }

    let metadata = load_blas_metadata_common(blasHandle);
    if metadata.handleId == INVALID_INDEX || metadata.cwbvhNodeCount == 0u {
        return false;
    }
    let primitiveMetadata = load_trace_primitive_metadata(blasHandle);

    let inverseTransform = instance.inverseTransform;
    let localOrigin = (inverseTransform * vec4<f32>(rayOrigin, 1.0)).xyz;
    let localDirection = (inverseTransform * vec4<f32>(rayDirection, 0.0)).xyz;
    if any(localOrigin != localOrigin) || any(localDirection != localDirection) || dot(localDirection, localDirection) <= 1.0e-12 {
        return false;
    }

    let invdir = safe_inverse_direction(localDirection);
    var octinv = ((select(0u, 4u, localDirection.x < 0.0)) | (select(0u, 2u, localDirection.y < 0.0)) | (select(0u, 1u, localDirection.z < 0.0)));
    octinv = 7u - octinv;

    var traversalStack: array<vec4<u32>, TLAS_TRAVERSAL_STACK_SIZE>;
    var stackPtr: u32 = 0u;
    var ngroup = vec2<u32>(0u, 0x80000000u);
    var ngroupMinT = 0.0;
    var tgroup = vec2<u32>(0u, 0u);
    var tgroupMinT = maxRayT;
    let orig = localOrigin;
    let tmin = uniforms.tMin;
    let tmax = maxRayT;

    loop {
        if (ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) > 0x00FFFFFFu && ngroupMinT > maxRayT {
            ngroup = vec2<u32>(0u, 0u);
        }

        if (ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) > 0x00FFFFFFu {
            let parentUsesSlotOrder = (ngroup.y & TLAS_GROUP_SLOT_ORDER_MASK_FLAG) != 0u;
            let imask = ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG;
            let parentOctinv = select(octinv, 0u, parentUsesSlotOrder);
            let childBitIndex = most_significant_bit_index(imask);
            let childNodeBaseIndex = ngroup.x;

            let remainingMask = imask & ~(1u << childBitIndex);
            ngroup.y = remainingMask | select(0u, TLAS_GROUP_SLOT_ORDER_MASK_FLAG, parentUsesSlotOrder && remainingMask > 0x00FFFFFFu);
            if (ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) > 0x00FFFFFFu {
                _ = push_tlas_stack_entry(&traversalStack, &stackPtr, ngroup.x, ngroup.y, ngroupMinT);
            }

            let slotIndex: u32 = (childBitIndex - 24u) ^ parentOctinv;
            let childNodeIndex: u32 = childNodeBaseIndex + countOneBits(imask & ~(0xFFFFFFFFu << slotIndex));
            let globalNodeIndex = metadata.cwbvhNodeOffset + childNodeIndex;
            let nodeBase = globalNodeIndex * 5u;
            let nodeUsesDobb = blas_dobb_node_active(globalNodeIndex);
            let nodeRotationIndex = select(0u, blasNodeMetadata[globalNodeIndex].rotationIndex, nodeUsesDobb);
            let n0 = select(blasNodes[nodeBase + 0u], blasDobbNodes[nodeBase + 0u], nodeUsesDobb);
            let n1 = select(blasNodes[nodeBase + 1u], blasDobbNodes[nodeBase + 1u], nodeUsesDobb);
            let n2 = select(blasNodes[nodeBase + 2u], blasDobbNodes[nodeBase + 2u], nodeUsesDobb);
            let n3 = select(blasNodes[nodeBase + 3u], blasDobbNodes[nodeBase + 3u], nodeUsesDobb);
            let n4 = select(blasNodes[nodeBase + 4u], blasDobbNodes[nodeBase + 4u], nodeUsesDobb);
            let nodeRayOrigin = select(localOrigin, tlas_dobb_rotate_vector(localOrigin, nodeRotationIndex), nodeUsesDobb);
            let nodeRayDirection = select(localDirection, tlas_dobb_rotate_vector(localDirection, nodeRotationIndex), nodeUsesDobb);
            let nodeInvdir = safe_inverse_direction(nodeRayDirection);
            let nodeOctinv = select(octinv, 0u, nodeUsesDobb);
            let p = vec3<f32>(bitcast<f32>(n0.x), bitcast<f32>(n0.y), bitcast<f32>(n0.z));
            let e_imask = unpack4xU8(n0.w);
            let octinv4 = nodeOctinv * 0x01010101u;
            let adjusted_idirx: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.x) + 127) << 23u) * nodeInvdir.x;
            let adjusted_idiry: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.y) + 127) << 23u) * nodeInvdir.y;
            let adjusted_idirz: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.z) + 127) << 23u) * nodeInvdir.z;
            let origx: f32 = -(nodeRayOrigin.x - p.x) * nodeInvdir.x;
            let origy: f32 = -(nodeRayOrigin.y - p.y) * nodeInvdir.y;
            let origz: f32 = -(nodeRayOrigin.z - p.z) * nodeInvdir.z;
            var hitmask = 0u;
            var childHitBits0 = vec4<u32>(0u);
            var childHitBits1 = vec4<u32>(0u);
            var childMinT0 = vec4<f32>(maxRayT);
            var childMinT1 = vec4<f32>(maxRayT);

            {
                let meta4: u32 = n1.z;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.x, n3.z, nodeInvdir.x < 0.0);
                    let swizzledHix = select(n3.z, n2.x, nodeInvdir.x < 0.0);
                    let swizzledLoy = select(n2.z, n4.x, nodeInvdir.y < 0.0);
                    let swizzledHiy = select(n4.x, n2.z, nodeInvdir.y < 0.0);
                    let swizzledLoz = select(n3.x, n4.z, nodeInvdir.z < 0.0);
                    let swizzledHiz = select(n4.z, n3.x, nodeInvdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits0[i] = hitBits;
                                childMinT0[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }
            {
                let meta4: u32 = n1.w;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.y, n3.w, nodeInvdir.x < 0.0);
                    let swizzledHix = select(n3.w, n2.y, nodeInvdir.x < 0.0);
                    let swizzledLoy = select(n2.w, n4.y, nodeInvdir.y < 0.0);
                    let swizzledHiy = select(n4.y, n2.w, nodeInvdir.y < 0.0);
                    let swizzledLoz = select(n3.y, n4.w, nodeInvdir.z < 0.0);
                    let swizzledHiz = select(n4.w, n3.y, nodeInvdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits1[i] = hitBits;
                                childMinT1[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }

            var bestBaseIndex = 0u;
            var bestMask = 0u;
            var bestMinT = maxRayT;
            var bestIsInternal = false;
            let internalGroupMask = (hitmask & 0xFF000000u) | e_imask.w;
            let internalPrefixMask = internalGroupMask & 0x00FFFFFFu;
            for (var childSlot = 0u; childSlot < 8u; childSlot += 1u) {
                var hitBits = 0u;
                var childMinT = maxRayT;
                if childSlot < 4u {
                    hitBits = childHitBits0[childSlot];
                    childMinT = childMinT0[childSlot];
                } else {
                    hitBits = childHitBits1[childSlot - 4u];
                    childMinT = childMinT1[childSlot - 4u];
                }
                if hitBits == 0u {
                    continue;
                }
                let childIsInternal = (hitBits & 0xFF000000u) != 0u;
                let baseIndex = select(n1.y, n1.x, childIsInternal);
                let scheduledMaskBase = select(hitBits, hitBits | internalPrefixMask, childIsInternal);
                let scheduledMask = scheduledMaskBase | select(0u, TLAS_GROUP_SLOT_ORDER_MASK_FLAG, nodeUsesDobb && childIsInternal);
                if bestMask == 0u || childMinT < bestMinT {
                    if bestMask != 0u {
                        _ = push_tlas_stack_entry(&traversalStack, &stackPtr, bestBaseIndex, bestMask, bestMinT);
                    }
                    bestBaseIndex = baseIndex;
                    bestMask = scheduledMask;
                    bestMinT = childMinT;
                    bestIsInternal = childIsInternal;
                } else {
                    _ = push_tlas_stack_entry(&traversalStack, &stackPtr, baseIndex, scheduledMask, childMinT);
                }
            }
            if bestMask == 0u {
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = maxRayT;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = maxRayT;
            } else if bestIsInternal {
                ngroup = vec2<u32>(bestBaseIndex, bestMask);
                ngroupMinT = bestMinT;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = maxRayT;
            } else {
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = maxRayT;
                tgroup = vec2<u32>(bestBaseIndex, bestMask);
                tgroupMinT = bestMinT;
            }
        } else {
            tgroup = ngroup;
            tgroupMinT = ngroupMinT;
            ngroup = vec2<u32>(0u, 0u);
        }

        if tgroup.y != 0u && tgroupMinT <= maxRayT {
            while tgroup.y != 0u {
                let primitiveBit = most_significant_bit_index(tgroup.y);
                let localPrimitiveIndex = load_local_leaf_remap(metadata.leafRemapOffset + tgroup.x + primitiveBit);
                if trace_blas_primitive_leaf_any_hit_cached(rayOrigin, rayDirection, instance, metadata, primitiveMetadata, localPrimitiveIndex, localOrigin, localDirection, invdir, maxRayT) {
                    return true;
                }
                tgroup.y &= ~(1u << primitiveBit);
            }
        }

        if (ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) <= 0x00FFFFFFu {
            let entry = pop_nearest_stack_entry(&traversalStack, &stackPtr, maxRayT, maxRayT, false, 0.0);
            if entry.y != 0u {
                ngroup = entry.xy;
                ngroupMinT = bitcast<f32>(entry.z);
            } else {
                return false;
            }
        }
    }

    return false;
}

#ifdef SWRT_WORLD_CWBVH
fn trace_world_triangle_leaf(
    rayOrigin: vec3<f32>,
    rayDirection: vec3<f32>,
    instanceIndex: u32,
    worldTriangleIndex: u32,
    originalPrimitiveIndex: u32,
    state: ptr<function, HitState>) {
    (*state).iterationCount += 1u;
    if worldTriangleIndex == INVALID_INDEX || originalPrimitiveIndex == INVALID_INDEX {
        return;
    }

    trace_stat_add_triangle_test(1u);
    let transform = blasTriangleTransforms[worldTriangleIndex];
    let hit = intersect_triangle_transform(rayOrigin, rayDirection, transform);
    if hit.z < 0.0 {
        return;
    }

    let worldT = hit.z;
    if worldT < uniforms.tMin || worldT > uniforms.tMax || worldT >= (*state).bestT {
        return;
    }

    update_best_hit_local(state, worldT, hit.xy, originalPrimitiveIndex, instanceIndex);
}

fn push_world_cwbvh_stack_entry(
    traversalStack: ptr<function, array<vec4<u32>, TLAS_TRAVERSAL_STACK_SIZE>>,
    stackPtr: ptr<function, u32>,
    baseIndex: u32,
    mask: u32,
    minT: f32,
    ownerInstanceIndex: u32) -> bool {
    if (*stackPtr) >= TLAS_TRAVERSAL_STACK_SIZE {
        trace_stat_add_local_stack_spill(1u);
        return false;
    }
    (*traversalStack)[(*stackPtr)] = vec4<u32>(baseIndex, mask, bitcast<u32>(minT), ownerInstanceIndex);
    (*stackPtr) += 1u;
    return true;
}

fn trace_stat_add_world_cwbvh_node_visit(ownerInstanceIndex: u32) {
#ifdef SWRT_ENABLE_TRACE_STATS
    if ownerInstanceIndex == INVALID_INDEX {
        atomicAdd(&traceStats.tlasNodeVisits, 1u);
    } else {
        atomicAdd(&traceStats.blasNodeVisits, 1u);
        atomicAdd(&traceStats.worldBlasNodeVisits, 1u);
    }
#endif
}

fn trace_world_cwbvh(rayOrigin: vec3<f32>, rayDirection: vec3<f32>, state: ptr<function, HitState>) {
    if uniforms.tlasNodeCount == 0u {
        return;
    }

    let invdir = safe_inverse_direction(rayDirection);
    var octinv = 7u - ((select(0u, 4u, rayDirection.x < 0.0)) | (select(0u, 2u, rayDirection.y < 0.0)) | (select(0u, 1u, rayDirection.z < 0.0)));
    var traversalStack: array<vec4<u32>, TLAS_TRAVERSAL_STACK_SIZE>;
    var stackPtr = 0u;
    var ngroup = vec2<u32>(0u, 0x80000000u);
    var ngroupMinT = 0.0;
    var ngroupOwnerInstance = INVALID_INDEX;
    var tgroup = vec2<u32>(0u, 0u);
    var tgroupMinT = uniforms.tMax;
    var tgroupOwnerInstance = INVALID_INDEX;
#ifdef SWRT_WORLD_CWBVH_SUBGROUP
    var triangleGroupPostponed = false;
#endif
    let orig = rayOrigin;
    let tmin = uniforms.tMin;
    var tmax = min((*state).bestT, uniforms.tMax);
    let earlyOut = traversal_early_out_enabled();
    let earlyOutBias = max(uniforms.traceControl.y, 0.0);

    loop {
        if ngroup.y > 0x00FFFFFFu && should_skip_group_fast(ngroupMinT, (*state).bestT, earlyOut, earlyOutBias) {
            ngroup = vec2<u32>(0u, 0u);
            ngroupOwnerInstance = INVALID_INDEX;
        }

        if ngroup.y > 0x00FFFFFFu {
            (*state).iterationCount += 1u;
            trace_stat_add_world_cwbvh_node_visit(ngroupOwnerInstance);
            let imask = ngroup.y;
            let childBitIndex = most_significant_bit_index(ngroup.y);
            let childNodeBaseIndex = ngroup.x;

            ngroup.y &= ~(1u << childBitIndex);
            if ngroup.y > 0x00FFFFFFu {
                if !push_world_cwbvh_stack_entry(&traversalStack, &stackPtr, ngroup.x, ngroup.y, ngroupMinT, ngroupOwnerInstance) {
                    return;
                }
            }

            let slotIndex: u32 = (childBitIndex - 24u) ^ octinv;
            let childNodeIndex: u32 = childNodeBaseIndex + countOneBits(imask & ~(0xFFFFFFFFu << slotIndex));
            let nodeBase = childNodeIndex * 5u;
            let n0 = tlasNodes[nodeBase + 0u];
            let n1 = tlasNodes[nodeBase + 1u];
            let n2 = tlasNodes[nodeBase + 2u];
            let n3 = tlasNodes[nodeBase + 3u];
            let n4 = tlasNodes[nodeBase + 4u];
            let p = vec3<f32>(bitcast<f32>(n0.x), bitcast<f32>(n0.y), bitcast<f32>(n0.z));
            let e_imask = unpack4xU8(n0.w);
            let octinv4 = octinv * 0x01010101u;
            let adjusted_idirx: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.x) + 127) << 23u) * invdir.x;
            let adjusted_idiry: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.y) + 127) << 23u) * invdir.y;
            let adjusted_idirz: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.z) + 127) << 23u) * invdir.z;
            let origx: f32 = -(orig.x - p.x) * invdir.x;
            let origy: f32 = -(orig.y - p.y) * invdir.y;
            let origz: f32 = -(orig.z - p.z) * invdir.z;
            var hitmask = 0u;
            var childHitBits0 = vec4<u32>(0u);
            var childHitBits1 = vec4<u32>(0u);
            var childMinT0 = vec4<f32>(uniforms.tMax);
            var childMinT1 = vec4<f32>(uniforms.tMax);

            {
                let meta4: u32 = n1.z;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.x, n3.z, invdir.x < 0.0);
                    let swizzledHix = select(n3.z, n2.x, invdir.x < 0.0);
                    let swizzledLoy = select(n2.z, n4.x, invdir.y < 0.0);
                    let swizzledHiy = select(n4.x, n2.z, invdir.y < 0.0);
                    let swizzledLoz = select(n3.x, n4.z, invdir.z < 0.0);
                    let swizzledHiz = select(n4.z, n3.x, invdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits0[i] = hitBits;
                                childMinT0[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }
            {
                let meta4: u32 = n1.w;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.y, n3.w, invdir.x < 0.0);
                    let swizzledHix = select(n3.w, n2.y, invdir.x < 0.0);
                    let swizzledLoy = select(n2.w, n4.y, invdir.y < 0.0);
                    let swizzledHiy = select(n4.y, n2.w, invdir.y < 0.0);
                    let swizzledLoz = select(n3.y, n4.w, invdir.z < 0.0);
                    let swizzledHiz = select(n4.w, n3.y, invdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits1[i] = hitBits;
                                childMinT1[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }

            var bestBaseIndex = 0u;
            var bestMask = 0u;
            var bestMinT = uniforms.tMax;
            var bestIsInternal = false;
            let internalGroupMask = (hitmask & 0xFF000000u) | e_imask.w;
            let internalPrefixMask = internalGroupMask & 0x00FFFFFFu;
            for (var childSlot = 0u; childSlot < 8u; childSlot += 1u) {
                var hitBits = 0u;
                var childMinT = uniforms.tMax;
                if childSlot < 4u {
                    hitBits = childHitBits0[childSlot];
                    childMinT = childMinT0[childSlot];
                } else {
                    hitBits = childHitBits1[childSlot - 4u];
                    childMinT = childMinT1[childSlot - 4u];
                }
                if hitBits == 0u {
                    continue;
                }
                let childIsInternal = (hitBits & 0xFF000000u) != 0u;
                let baseIndex = select(n1.y, n1.x, childIsInternal);
                let scheduledMask = select(hitBits, hitBits | internalPrefixMask, childIsInternal);
                if bestMask == 0u || childMinT < bestMinT {
                    if bestMask != 0u && !push_world_cwbvh_stack_entry(&traversalStack, &stackPtr, bestBaseIndex, bestMask, bestMinT, ngroupOwnerInstance) {
                        return;
                    }
                    bestBaseIndex = baseIndex;
                    bestMask = scheduledMask;
                    bestMinT = childMinT;
                    bestIsInternal = childIsInternal;
                } else if !push_world_cwbvh_stack_entry(&traversalStack, &stackPtr, baseIndex, scheduledMask, childMinT, ngroupOwnerInstance) {
                    return;
                }
            }
            if bestMask == 0u {
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = uniforms.tMax;
                ngroupOwnerInstance = INVALID_INDEX;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = uniforms.tMax;
                tgroupOwnerInstance = INVALID_INDEX;
            } else if bestIsInternal {
                ngroup = vec2<u32>(bestBaseIndex, bestMask);
                ngroupMinT = bestMinT;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = uniforms.tMax;
                tgroupOwnerInstance = INVALID_INDEX;
            } else {
                let leafOwnerInstance = ngroupOwnerInstance;
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = uniforms.tMax;
                ngroupOwnerInstance = INVALID_INDEX;
                tgroup = vec2<u32>(bestBaseIndex, bestMask);
                tgroupMinT = bestMinT;
                tgroupOwnerInstance = leafOwnerInstance;
            }
        } else {
            tgroup = ngroup;
            tgroupMinT = ngroupMinT;
            tgroupOwnerInstance = ngroupOwnerInstance;
            ngroup = vec2<u32>(0u, 0u);
            ngroupOwnerInstance = INVALID_INDEX;
        }

        if tgroup.y != 0u && should_skip_group_fast(tgroupMinT, (*state).bestT, earlyOut, earlyOutBias) {
            tgroup.y = 0u;
            tgroupOwnerInstance = INVALID_INDEX;
        } else {
#ifdef SWRT_WORLD_CWBVH_SUBGROUP
            let subgroupTraversalLaneCount = swrt_subgroup_active_lane_count(true);
#endif
            if tgroupOwnerInstance == INVALID_INDEX {
                var bestPendingGroup = vec2<u32>(0u, 0u);
                var bestPendingMinT = uniforms.tMax;
                var bestPendingOwner = INVALID_INDEX;
                while tgroup.y != 0u {
                    let leafBit = most_significant_bit_index(tgroup.y);
                    let instanceIndex = tlasLeafInstances[tgroup.x + leafBit];
                    if instanceIndex != INVALID_INDEX {
                        let range = worldInstanceRanges[instanceIndex];
                        if range.valid != 0u && range.blasHandle != INVALID_INDEX && range.worldCWBVHNodeCount != 0u {
                            let leaf = tlasInstanceAabbs[instanceIndex];
                            let worldInterval = intersect_aabb_interval_invdir(rayOrigin, invdir, leaf.minPoint, leaf.maxPoint);
                            let nearT = max(worldInterval.x, uniforms.tMin);
                            let farT = min(worldInterval.y, min((*state).bestT, uniforms.tMax));
                            if nearT <= farT && nearT <= (*state).bestT {
                                let candidateGroup = vec2<u32>(range.worldCWBVHNodeOffset, 0x80000000u);
                                if bestPendingGroup.y == 0u || nearT < bestPendingMinT {
                                    if bestPendingGroup.y != 0u && !push_world_cwbvh_stack_entry(&traversalStack, &stackPtr, bestPendingGroup.x, bestPendingGroup.y, bestPendingMinT, bestPendingOwner) {
                                        return;
                                    }
                                    bestPendingGroup = candidateGroup;
                                    bestPendingMinT = nearT;
                                    bestPendingOwner = instanceIndex;
                                } else if !push_world_cwbvh_stack_entry(&traversalStack, &stackPtr, candidateGroup.x, candidateGroup.y, nearT, instanceIndex) {
                                    return;
                                }
                            }
                        }
                    }
                    tgroup.y &= ~(1u << leafBit);
                }
                if bestPendingGroup.y != 0u {
                    ngroup = bestPendingGroup;
                    ngroupMinT = bestPendingMinT;
                    ngroupOwnerInstance = bestPendingOwner;
                }
            } else {
#ifdef SWRT_WORLD_CWBVH_SUBGROUP
                let subgroupTriangleLaneCount = swrt_subgroup_active_lane_count(tgroup.y != 0u);
                let subgroupTrianglePostponeThreshold = subgroupTraversalLaneCount / SWRT_TRIANGLE_POSTPONING_THRESHOLD_DIVISOR;
                if subgroupTrianglePostponeThreshold > 0u &&
                    subgroupTriangleLaneCount < subgroupTrianglePostponeThreshold &&
                    tgroup.y != 0u &&
                    !triangleGroupPostponed {
                    if !push_world_cwbvh_stack_entry(&traversalStack, &stackPtr, tgroup.x, tgroup.y, tgroupMinT, tgroupOwnerInstance) {
                        return;
                    }
                    triangleGroupPostponed = true;
                    tgroup.y = 0u;
                } else {
#endif
                    while tgroup.y != 0u {
                        let primitiveBit = most_significant_bit_index(tgroup.y);
                        let leaf = blasLeafRemap[tgroup.x + primitiveBit];
                        trace_world_triangle_leaf(rayOrigin, rayDirection, tgroupOwnerInstance, leaf.x, leaf.y, state);
                        tgroup.y &= ~(1u << primitiveBit);
                    }
#ifdef SWRT_WORLD_CWBVH_SUBGROUP
                    triangleGroupPostponed = false;
                }
#endif
                tmax = min((*state).bestT, uniforms.tMax);
            }
        }

        if (ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) <= 0x00FFFFFFu {
            let entry = pop_nearest_stack_entry(&traversalStack, &stackPtr, uniforms.tMax, (*state).bestT, earlyOut, earlyOutBias);
            if entry.y != 0u {
                ngroup = entry.xy;
                ngroupMinT = bitcast<f32>(entry.z);
                ngroupOwnerInstance = entry.w;
            } else {
                return;
            }
        }
    }
}
#endif

#ifndef SWRT_WORLD_CWBVH
fn trace_tlas_local(rayOrigin: vec3<f32>, rayDirection: vec3<f32>, state: ptr<function, HitState>) {
    if uniforms.tlasNodeCount == 0u {
        return;
    }

    let invdir = safe_inverse_direction(rayDirection);
    var octinv = 7u - ((select(0u, 4u, rayDirection.x < 0.0)) | (select(0u, 2u, rayDirection.y < 0.0)) | (select(0u, 1u, rayDirection.z < 0.0)));
    var traversalStack: array<vec4<u32>, TLAS_TRAVERSAL_STACK_SIZE>;
    var stackPtr = 0u;
    var ngroup = vec2<u32>(0u, 0x80000000u);
    var ngroupMinT = 0.0;
    var tgroup = vec2<u32>(0u, 0u);
    var tgroupMinT = uniforms.tMax;
    let orig = rayOrigin;
    let tmin = uniforms.tMin;
    var tmax = min((*state).bestT, uniforms.tMax);
    let earlyOut = traversal_early_out_enabled();
    let earlyOutBias = max(uniforms.traceControl.y, 0.0);

    loop {
        if (ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) > 0x00FFFFFFu && should_skip_group_fast(ngroupMinT, (*state).bestT, earlyOut, earlyOutBias) {
            ngroup = vec2<u32>(0u, 0u);
        }

        if (ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) > 0x00FFFFFFu {
            (*state).iterationCount += 1u;
            let parentUsesSlotOrder = (ngroup.y & TLAS_GROUP_SLOT_ORDER_MASK_FLAG) != 0u;
            let imask = ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG;
            let parentOctinv = select(octinv, 0u, parentUsesSlotOrder);
            let childBitIndex = most_significant_bit_index(imask);
            let childNodeBaseIndex = ngroup.x;

            let remainingMask = imask & ~(1u << childBitIndex);
            ngroup.y = remainingMask | select(0u, TLAS_GROUP_SLOT_ORDER_MASK_FLAG, parentUsesSlotOrder && remainingMask > 0x00FFFFFFu);
            if (ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) > 0x00FFFFFFu {
                if !push_tlas_stack_entry(&traversalStack, &stackPtr, ngroup.x, ngroup.y, ngroupMinT) {
                    return;
                }
            }

            let slotIndex: u32 = (childBitIndex - 24u) ^ parentOctinv;
            let childNodeIndex: u32 = childNodeBaseIndex + countOneBits(imask & ~(0xFFFFFFFFu << slotIndex));
            let nodeBase = childNodeIndex * 5u;
            let nodeUsesDobb = tlas_dobb_node_active(childNodeIndex);
            let nodeRotationIndex = select(0u, tlasNodeMetadata[childNodeIndex].rotationIndex, nodeUsesDobb);
            let n0 = select(tlasNodes[nodeBase + 0u], tlasDobbNodes[nodeBase + 0u], nodeUsesDobb);
            let n1 = select(tlasNodes[nodeBase + 1u], tlasDobbNodes[nodeBase + 1u], nodeUsesDobb);
            let n2 = select(tlasNodes[nodeBase + 2u], tlasDobbNodes[nodeBase + 2u], nodeUsesDobb);
            let n3 = select(tlasNodes[nodeBase + 3u], tlasDobbNodes[nodeBase + 3u], nodeUsesDobb);
            let n4 = select(tlasNodes[nodeBase + 4u], tlasDobbNodes[nodeBase + 4u], nodeUsesDobb);
            let nodeRayOrigin = select(rayOrigin, tlas_dobb_rotate_vector(rayOrigin, nodeRotationIndex), nodeUsesDobb);
            let nodeRayDirection = select(rayDirection, tlas_dobb_rotate_vector(rayDirection, nodeRotationIndex), nodeUsesDobb);
            let nodeInvdir = safe_inverse_direction(nodeRayDirection);
            let nodeOctinv = select(octinv, 0u, nodeUsesDobb);
            let p = vec3<f32>(bitcast<f32>(n0.x), bitcast<f32>(n0.y), bitcast<f32>(n0.z));
            let e_imask = unpack4xU8(n0.w);
            let octinv4 = nodeOctinv * 0x01010101u;
            let adjusted_idirx: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.x) + 127) << 23u) * nodeInvdir.x;
            let adjusted_idiry: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.y) + 127) << 23u) * nodeInvdir.y;
            let adjusted_idirz: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.z) + 127) << 23u) * nodeInvdir.z;
            let origx: f32 = -(nodeRayOrigin.x - p.x) * nodeInvdir.x;
            let origy: f32 = -(nodeRayOrigin.y - p.y) * nodeInvdir.y;
            let origz: f32 = -(nodeRayOrigin.z - p.z) * nodeInvdir.z;
            var hitmask = 0u;
            var childHitBits0 = vec4<u32>(0u);
            var childHitBits1 = vec4<u32>(0u);
            var childMinT0 = vec4<f32>(uniforms.tMax);
            var childMinT1 = vec4<f32>(uniforms.tMax);

            {
                let meta4: u32 = n1.z;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.x, n3.z, nodeInvdir.x < 0.0);
                    let swizzledHix = select(n3.z, n2.x, nodeInvdir.x < 0.0);
                    let swizzledLoy = select(n2.z, n4.x, nodeInvdir.y < 0.0);
                    let swizzledHiy = select(n4.x, n2.z, nodeInvdir.y < 0.0);
                    let swizzledLoz = select(n3.x, n4.z, nodeInvdir.z < 0.0);
                    let swizzledHiz = select(n4.z, n3.x, nodeInvdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits0[i] = hitBits;
                                childMinT0[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }
            {
                let meta4: u32 = n1.w;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.y, n3.w, nodeInvdir.x < 0.0);
                    let swizzledHix = select(n3.w, n2.y, nodeInvdir.x < 0.0);
                    let swizzledLoy = select(n2.w, n4.y, nodeInvdir.y < 0.0);
                    let swizzledHiy = select(n4.y, n2.w, nodeInvdir.y < 0.0);
                    let swizzledLoz = select(n3.y, n4.w, nodeInvdir.z < 0.0);
                    let swizzledHiz = select(n4.w, n3.y, nodeInvdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits1[i] = hitBits;
                                childMinT1[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }

            var bestBaseIndex = 0u;
            var bestMask = 0u;
            var bestMinT = uniforms.tMax;
            var bestIsInternal = false;
            let internalGroupMask = (hitmask & 0xFF000000u) | e_imask.w;
            let internalPrefixMask = internalGroupMask & 0x00FFFFFFu;
            for (var childSlot = 0u; childSlot < 8u; childSlot += 1u) {
                var hitBits = 0u;
                var childMinT = uniforms.tMax;
                if childSlot < 4u {
                    hitBits = childHitBits0[childSlot];
                    childMinT = childMinT0[childSlot];
                } else {
                    hitBits = childHitBits1[childSlot - 4u];
                    childMinT = childMinT1[childSlot - 4u];
                }
                if hitBits == 0u {
                    continue;
                }
                let childIsInternal = (hitBits & 0xFF000000u) != 0u;
                let baseIndex = select(n1.y, n1.x, childIsInternal);
                let scheduledMaskBase = select(hitBits, hitBits | internalPrefixMask, childIsInternal);
                let scheduledMask = scheduledMaskBase | select(0u, TLAS_GROUP_SLOT_ORDER_MASK_FLAG, nodeUsesDobb && childIsInternal);
                if bestMask == 0u || childMinT < bestMinT {
                    if bestMask != 0u && !push_tlas_stack_entry(&traversalStack, &stackPtr, bestBaseIndex, bestMask, bestMinT) {
                        return;
                    }
                    bestBaseIndex = baseIndex;
                    bestMask = scheduledMask;
                    bestMinT = childMinT;
                    bestIsInternal = childIsInternal;
                } else if !push_tlas_stack_entry(&traversalStack, &stackPtr, baseIndex, scheduledMask, childMinT) {
                    return;
                }
            }
            if bestMask == 0u {
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = uniforms.tMax;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = uniforms.tMax;
            } else if bestIsInternal {
                ngroup = vec2<u32>(bestBaseIndex, bestMask);
                ngroupMinT = bestMinT;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = uniforms.tMax;
            } else {
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = uniforms.tMax;
                tgroup = vec2<u32>(bestBaseIndex, bestMask);
                tgroupMinT = bestMinT;
            }
        } else {
            tgroup = ngroup;
            tgroupMinT = ngroupMinT;
            ngroup = vec2<u32>(0u, 0u);
        }

        if tgroup.y != 0u && should_skip_group_fast(tgroupMinT, (*state).bestT, earlyOut, earlyOutBias) {
            tgroup.y = 0u;
        } else {
            while tgroup.y != 0u {
                let leafBit = most_significant_bit_index(tgroup.y);
                let instanceIndex = tlasLeafInstances[tgroup.x + leafBit];
                if instanceIndex != INVALID_INDEX {
                    let leaf = tlasInstanceAabbs[instanceIndex];
                    let worldInterval = intersect_aabb_interval_invdir(rayOrigin, invdir, leaf.minPoint, leaf.maxPoint);
                    let nearT = max(worldInterval.x, uniforms.tMin);
                    let farT = min(worldInterval.y, min((*state).bestT, uniforms.tMax));
                    if leaf.blasHandle != INVALID_INDEX && nearT <= farT {
                        trace_instance_blas_local(
                            rayOrigin,
                            rayDirection,
                            leaf.instanceIndex,
                            nearT,
                            vec2<u32>(0u, 0x80000000u),
                            state);
                        tmax = min(tmax, (*state).bestT);
                    }
                }
                tgroup.y &= ~(1u << leafBit);
            }
        }

        if (ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) <= 0x00FFFFFFu {
            let entry = pop_nearest_stack_entry(&traversalStack, &stackPtr, uniforms.tMax, (*state).bestT, earlyOut, earlyOutBias);
            if entry.y != 0u {
                ngroup = entry.xy;
                ngroupMinT = bitcast<f32>(entry.z);
            } else {
                return;
            }
        }
    }
}

fn two_stage_candidate_capacity() -> u32 {
    return clamp(uniforms.twoStageControl.x, 1u, TLAS_BLAS_CANDIDATE_STACK_SIZE);
}

fn two_stage_blas_test_limit() -> u32 {
    return clamp(uniforms.twoStageControl.y, 1u, two_stage_candidate_capacity());
}

fn blas_candidate_effective_count(rawCount: u32) -> u32 {
    return rawCount & ~BLAS_CANDIDATE_COUNT_OVERFLOW_BIT;
}

fn blas_candidate_has_overflow(rawCount: u32) -> bool {
    return (rawCount & BLAS_CANDIDATE_COUNT_OVERFLOW_BIT) != 0u;
}

fn invalid_blas_candidate() -> BLASCandidate {
    return BLASCandidate(INVALID_INDEX, INVALID_INDEX, uniforms.tMax, 0u);
}

fn push_tlas_blas_candidate(
    localCandidates: ptr<function, array<BLASCandidate, TLAS_BLAS_CANDIDATE_STACK_SIZE>>,
    candidateCount: ptr<function, u32>,
    candidateOverflow: ptr<function, bool>,
    instanceIndex: u32,
    blasHandle: u32,
    nearT: f32) {
    if (*candidateCount) >= two_stage_candidate_capacity() {
        (*candidateOverflow) = true;
        trace_stat_add_local_stack_spill(1u);
        trace_stat_add_two_stage_candidate_overflow();
        return;
    }

    (*localCandidates)[(*candidateCount)] = BLASCandidate(instanceIndex, blasHandle, nearT, 1u);
    (*candidateCount) += 1u;
}

fn write_tlas_blas_candidates(rayId: u32, localCandidates: ptr<function, array<BLASCandidate, TLAS_BLAS_CANDIDATE_STACK_SIZE>>, candidateCount: u32, candidateOverflow: bool) {
    let capacity = two_stage_candidate_capacity();
    blasCandidateCounts[rayId] = candidateCount | select(0u, BLAS_CANDIDATE_COUNT_OVERFLOW_BIT, candidateOverflow);
    let base = rayId * capacity;
    for (var i = 0u; i < TLAS_BLAS_CANDIDATE_STACK_SIZE; i += 1u) {
        if i >= capacity {
            break;
        }
        if i < candidateCount {
            blasCandidates[base + i] = (*localCandidates)[i];
        } else {
            blasCandidates[base + i] = invalid_blas_candidate();
        }
    }
}

fn sort_local_blas_candidates_by_distance(
    localCandidates: ptr<function, array<BLASCandidate, TLAS_BLAS_CANDIDATE_STACK_SIZE>>,
    candidateCount: u32) {
    if candidateCount <= 1u {
        return;
    }

    trace_stat_add_two_stage_candidate_sort();
    for (var i = 1u; i < TLAS_BLAS_CANDIDATE_STACK_SIZE; i += 1u) {
        if i >= candidateCount {
            break;
        }
        let key = (*localCandidates)[i];
        var j = i;
        loop {
            if j == 0u {
                break;
            }
            let previous = (*localCandidates)[j - 1u];
            if previous.nearT <= key.nearT {
                break;
            }
            (*localCandidates)[j] = previous;
            j -= 1u;
        }
        (*localCandidates)[j] = key;
    }
}

fn trace_local_blas_candidates(
    rayId: u32,
    rayOrigin: vec3<f32>,
    rayDirection: vec3<f32>,
    rayTMax: f32,
    localCandidates: ptr<function, array<BLASCandidate, TLAS_BLAS_CANDIDATE_STACK_SIZE>>,
    candidateCount: u32,
    candidateOverflow: bool) {
    if candidateOverflow {
        var fallbackState = HitState(rayTMax, INVALID_INDEX, INVALID_INDEX, vec2<f32>(0.0), 0u);
        trace_tlas_local(rayOrigin, rayDirection, &fallbackState);
        store_trace_result(rayId, fallbackState);
        return;
    }

    sort_local_blas_candidates_by_distance(localCandidates, candidateCount);
    var hitState = HitState(rayTMax, INVALID_INDEX, INVALID_INDEX, vec2<f32>(0.0), 0u);
    var testedCandidates = 0u;
    for (var i = 0u; i < TLAS_BLAS_CANDIDATE_STACK_SIZE; i += 1u) {
        if i >= candidateCount {
            break;
        }

        let candidate = (*localCandidates)[i];
        if candidate.instanceIndex == INVALID_INDEX || candidate.blasHandle == INVALID_INDEX {
            continue;
        }
        if candidate.nearT > hitState.bestT {
            trace_stat_add_two_stage_distance_early_stop();
            break;
        }

        testedCandidates += 1u;
        trace_stat_add_two_stage_blas_candidate_test();
        trace_instance_blas_local(
            rayOrigin,
            rayDirection,
            candidate.instanceIndex,
            candidate.nearT,
            vec2<u32>(0u, 0x80000000u),
            &hitState);
    }

    store_trace_result(rayId, hitState);
    hitOutput[rayId]._padding0 = testedCandidates;
}

fn finish_tlas_blas_candidates(
    rayId: u32,
    rayOrigin: vec3<f32>,
    rayDirection: vec3<f32>,
    rayTMax: f32,
    localCandidates: ptr<function, array<BLASCandidate, TLAS_BLAS_CANDIDATE_STACK_SIZE>>,
    candidateCount: u32,
    candidateOverflow: bool,
    traceImmediately: bool) {
    trace_stat_add_two_stage_candidates_emitted(candidateCount);
    trace_stat_update_two_stage_max_candidates(candidateCount);
    if traceImmediately {
        trace_local_blas_candidates(rayId, rayOrigin, rayDirection, rayTMax, localCandidates, candidateCount, candidateOverflow);
    } else {
        write_tlas_blas_candidates(rayId, localCandidates, candidateCount, candidateOverflow);
    }
}

fn trace_tlas_blas_candidates_local(rayOrigin: vec3<f32>, rayDirection: vec3<f32>, rayTMax: f32, rayId: u32, traceImmediately: bool) {
    var localCandidates: array<BLASCandidate, TLAS_BLAS_CANDIDATE_STACK_SIZE>;
    var candidateCount = 0u;
    var candidateOverflow = false;

    if uniforms.tlasNodeCount == 0u {
        finish_tlas_blas_candidates(rayId, rayOrigin, rayDirection, rayTMax, &localCandidates, candidateCount, candidateOverflow, traceImmediately);
        return;
    }

    let invdir = safe_inverse_direction(rayDirection);
    var octinv = 7u - ((select(0u, 4u, rayDirection.x < 0.0)) | (select(0u, 2u, rayDirection.y < 0.0)) | (select(0u, 1u, rayDirection.z < 0.0)));
    var traversalStack: array<vec4<u32>, TLAS_TRAVERSAL_STACK_SIZE>;
    var stackPtr = 0u;
    var ngroup = vec2<u32>(0u, 0x80000000u);
    var ngroupMinT = 0.0;
    var tgroup = vec2<u32>(0u, 0u);
    var tgroupMinT = uniforms.tMax;
    let orig = rayOrigin;
    let tmin = uniforms.tMin;
    let tmax = uniforms.tMax;

    loop {
        if (ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) > 0x00FFFFFFu && ngroupMinT > uniforms.tMax {
            ngroup = vec2<u32>(0u, 0u);
        }

        if (ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) > 0x00FFFFFFu {
            let parentUsesSlotOrder = (ngroup.y & TLAS_GROUP_SLOT_ORDER_MASK_FLAG) != 0u;
            let imask = ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG;
            let parentOctinv = select(octinv, 0u, parentUsesSlotOrder);
            let childBitIndex = most_significant_bit_index(imask);
            let childNodeBaseIndex = ngroup.x;

            let remainingMask = imask & ~(1u << childBitIndex);
            ngroup.y = remainingMask | select(0u, TLAS_GROUP_SLOT_ORDER_MASK_FLAG, parentUsesSlotOrder && remainingMask > 0x00FFFFFFu);
            if (ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) > 0x00FFFFFFu {
                if !push_tlas_stack_entry(&traversalStack, &stackPtr, ngroup.x, ngroup.y, ngroupMinT) {
                    candidateOverflow = true;
                    trace_stat_add_two_stage_candidate_overflow();
                    break;
                }
            }

            let slotIndex: u32 = (childBitIndex - 24u) ^ parentOctinv;
            let childNodeIndex: u32 = childNodeBaseIndex + countOneBits(imask & ~(0xFFFFFFFFu << slotIndex));
            let nodeBase = childNodeIndex * 5u;
            let nodeUsesDobb = tlas_dobb_node_active(childNodeIndex);
            let nodeRotationIndex = select(0u, tlasNodeMetadata[childNodeIndex].rotationIndex, nodeUsesDobb);
            let n0 = select(tlasNodes[nodeBase + 0u], tlasDobbNodes[nodeBase + 0u], nodeUsesDobb);
            let n1 = select(tlasNodes[nodeBase + 1u], tlasDobbNodes[nodeBase + 1u], nodeUsesDobb);
            let n2 = select(tlasNodes[nodeBase + 2u], tlasDobbNodes[nodeBase + 2u], nodeUsesDobb);
            let n3 = select(tlasNodes[nodeBase + 3u], tlasDobbNodes[nodeBase + 3u], nodeUsesDobb);
            let n4 = select(tlasNodes[nodeBase + 4u], tlasDobbNodes[nodeBase + 4u], nodeUsesDobb);
            let nodeRayOrigin = select(rayOrigin, tlas_dobb_rotate_vector(rayOrigin, nodeRotationIndex), nodeUsesDobb);
            let nodeRayDirection = select(rayDirection, tlas_dobb_rotate_vector(rayDirection, nodeRotationIndex), nodeUsesDobb);
            let nodeInvdir = safe_inverse_direction(nodeRayDirection);
            let nodeOctinv = select(octinv, 0u, nodeUsesDobb);
            let p = vec3<f32>(bitcast<f32>(n0.x), bitcast<f32>(n0.y), bitcast<f32>(n0.z));
            let e_imask = unpack4xU8(n0.w);
            let octinv4 = nodeOctinv * 0x01010101u;
            let adjusted_idirx: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.x) + 127) << 23u) * nodeInvdir.x;
            let adjusted_idiry: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.y) + 127) << 23u) * nodeInvdir.y;
            let adjusted_idirz: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.z) + 127) << 23u) * nodeInvdir.z;
            let origx: f32 = -(nodeRayOrigin.x - p.x) * nodeInvdir.x;
            let origy: f32 = -(nodeRayOrigin.y - p.y) * nodeInvdir.y;
            let origz: f32 = -(nodeRayOrigin.z - p.z) * nodeInvdir.z;
            var hitmask = 0u;
            var childHitBits0 = vec4<u32>(0u);
            var childHitBits1 = vec4<u32>(0u);
            var childMinT0 = vec4<f32>(uniforms.tMax);
            var childMinT1 = vec4<f32>(uniforms.tMax);

            {
                let meta4: u32 = n1.z;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.x, n3.z, nodeInvdir.x < 0.0);
                    let swizzledHix = select(n3.z, n2.x, nodeInvdir.x < 0.0);
                    let swizzledLoy = select(n2.z, n4.x, nodeInvdir.y < 0.0);
                    let swizzledHiy = select(n4.x, n2.z, nodeInvdir.y < 0.0);
                    let swizzledLoz = select(n3.x, n4.z, nodeInvdir.z < 0.0);
                    let swizzledHiz = select(n4.z, n3.x, nodeInvdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits0[i] = hitBits;
                                childMinT0[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }
            {
                let meta4: u32 = n1.w;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.y, n3.w, nodeInvdir.x < 0.0);
                    let swizzledHix = select(n3.w, n2.y, nodeInvdir.x < 0.0);
                    let swizzledLoy = select(n2.w, n4.y, nodeInvdir.y < 0.0);
                    let swizzledHiy = select(n4.y, n2.w, nodeInvdir.y < 0.0);
                    let swizzledLoz = select(n3.y, n4.w, nodeInvdir.z < 0.0);
                    let swizzledHiz = select(n4.w, n3.y, nodeInvdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits1[i] = hitBits;
                                childMinT1[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }

            var bestBaseIndex = 0u;
            var bestMask = 0u;
            var bestMinT = uniforms.tMax;
            var bestIsInternal = false;
            let internalGroupMask = (hitmask & 0xFF000000u) | e_imask.w;
            let internalPrefixMask = internalGroupMask & 0x00FFFFFFu;
            for (var childSlot = 0u; childSlot < 8u; childSlot += 1u) {
                var hitBits = 0u;
                var childMinT = uniforms.tMax;
                if childSlot < 4u {
                    hitBits = childHitBits0[childSlot];
                    childMinT = childMinT0[childSlot];
                } else {
                    hitBits = childHitBits1[childSlot - 4u];
                    childMinT = childMinT1[childSlot - 4u];
                }
                if hitBits == 0u {
                    continue;
                }
                let childIsInternal = (hitBits & 0xFF000000u) != 0u;
                let baseIndex = select(n1.y, n1.x, childIsInternal);
                let scheduledMaskBase = select(hitBits, hitBits | internalPrefixMask, childIsInternal);
                let scheduledMask = scheduledMaskBase | select(0u, TLAS_GROUP_SLOT_ORDER_MASK_FLAG, nodeUsesDobb && childIsInternal);
                if bestMask == 0u || childMinT < bestMinT {
                    if bestMask != 0u && !push_tlas_stack_entry(&traversalStack, &stackPtr, bestBaseIndex, bestMask, bestMinT) {
                        candidateOverflow = true;
                        trace_stat_add_two_stage_candidate_overflow();
                        finish_tlas_blas_candidates(rayId, rayOrigin, rayDirection, rayTMax, &localCandidates, candidateCount, candidateOverflow, traceImmediately);
                        return;
                    }
                    bestBaseIndex = baseIndex;
                    bestMask = scheduledMask;
                    bestMinT = childMinT;
                    bestIsInternal = childIsInternal;
                } else if !push_tlas_stack_entry(&traversalStack, &stackPtr, baseIndex, scheduledMask, childMinT) {
                    candidateOverflow = true;
                    trace_stat_add_two_stage_candidate_overflow();
                    finish_tlas_blas_candidates(rayId, rayOrigin, rayDirection, rayTMax, &localCandidates, candidateCount, candidateOverflow, traceImmediately);
                    return;
                }
            }
            if bestMask == 0u {
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = uniforms.tMax;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = uniforms.tMax;
            } else if bestIsInternal {
                ngroup = vec2<u32>(bestBaseIndex, bestMask);
                ngroupMinT = bestMinT;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = uniforms.tMax;
            } else {
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = uniforms.tMax;
                tgroup = vec2<u32>(bestBaseIndex, bestMask);
                tgroupMinT = bestMinT;
            }
        } else {
            tgroup = ngroup;
            tgroupMinT = ngroupMinT;
            ngroup = vec2<u32>(0u, 0u);
        }

        if tgroup.y != 0u && tgroupMinT <= uniforms.tMax {
            while tgroup.y != 0u {
                let leafBit = most_significant_bit_index(tgroup.y);
                let instanceIndex = tlasLeafInstances[tgroup.x + leafBit];
                if instanceIndex != INVALID_INDEX {
                    let leaf = tlasInstanceAabbs[instanceIndex];
                    let worldInterval = intersect_aabb_interval_invdir(rayOrigin, invdir, leaf.minPoint, leaf.maxPoint);
                    let nearT = max(worldInterval.x, uniforms.tMin);
                    let farT = min(worldInterval.y, uniforms.tMax);
                    if leaf.blasHandle != INVALID_INDEX && nearT <= farT {
                        push_tlas_blas_candidate(&localCandidates, &candidateCount, &candidateOverflow, leaf.instanceIndex, leaf.blasHandle, nearT);
                    }
                }
                tgroup.y &= ~(1u << leafBit);
            }
        }

        if (ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) <= 0x00FFFFFFu {
            let entry = pop_nearest_stack_entry(&traversalStack, &stackPtr, uniforms.tMax, uniforms.tMax, false, 0.0);
            if entry.y != 0u {
                ngroup = entry.xy;
                ngroupMinT = bitcast<f32>(entry.z);
            } else {
                finish_tlas_blas_candidates(rayId, rayOrigin, rayDirection, rayTMax, &localCandidates, candidateCount, candidateOverflow, traceImmediately);
                return;
            }
        }
    }

    finish_tlas_blas_candidates(rayId, rayOrigin, rayDirection, rayTMax, &localCandidates, candidateCount, candidateOverflow, traceImmediately);
}

fn gather_tlas_blas_candidates_for_ray(rayId: u32, traceEnabled: bool) {
    if rayId >= uniforms.rayCount {
        return;
    }

    blasCandidateCounts[rayId] = 0u;
    if !traceEnabled {
        write_hit_output(rayId, INVALID_INDEX, INVALID_INDEX, 0.0, vec2<f32>(0.0), INVALID_INDEX, 0u);
        return;
    }

    trace_stat_begin_ray();
    let ray = rayInput[rayId];
    if !trace_ray_is_valid(ray) {
        trace_stat_add_invalid_ray();
        write_hit_output(rayId, INVALID_INDEX, INVALID_INDEX, 0.0, vec2<f32>(0.0), INVALID_INDEX, 0u);
        return;
    }

    trace_stat_begin_valid_ray();
    trace_tlas_blas_candidates_local(trace_ray_origin(ray), trace_ray_direction(ray), trace_ray_tmax(ray), rayId, false);
}

@compute @workgroup_size(WORKGROUP_SIZE_X, WORKGROUP_SIZE_Y, 1)
fn main_tlas_blas_candidates(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let traceEnabled = (uniforms.overlayFlags & OVERLAY_FLAG_TRACE_ENABLED) != 0u;
    let queueIndex = globalId.y * uniforms.rayDispatchWidth + globalId.x;
    var rayId = queueIndex;
    if uniforms.traceControl.w > 0.5 && queueIndex < uniforms.rayCount {
        rayId = raySortIds[queueIndex];
    }

    gather_tlas_blas_candidates_for_ray(rayId, traceEnabled);
}

fn sort_blas_candidates_for_ray(rayId: u32) {
    if rayId >= uniforms.rayCount {
        return;
    }

    let capacity = two_stage_candidate_capacity();
    let base = rayId * capacity;
    let count = min(blas_candidate_effective_count(blasCandidateCounts[rayId]), capacity);
    if count <= 1u {
        return;
    }

    trace_stat_add_two_stage_candidate_sort();
    for (var i = 1u; i < TLAS_BLAS_CANDIDATE_STACK_SIZE; i += 1u) {
        if i >= count {
            break;
        }
        let key = blasCandidates[base + i];
        var j = i;
        loop {
            if j == 0u {
                break;
            }
            let previous = blasCandidates[base + j - 1u];
            if previous.nearT <= key.nearT {
                break;
            }
            blasCandidates[base + j] = previous;
            j -= 1u;
        }
        blasCandidates[base + j] = key;
    }
}

@compute @workgroup_size(64, 1, 1)
fn sort_blas_candidates_by_distance(@builtin(global_invocation_id) globalId: vec3<u32>) {
    sort_blas_candidates_for_ray(globalId.x);
}

fn trace_sorted_blas_candidates_for_ray(rayId: u32, traceEnabled: bool) {
    if rayId >= uniforms.rayCount {
        return;
    }

    if !traceEnabled {
        write_hit_output(rayId, INVALID_INDEX, INVALID_INDEX, 0.0, vec2<f32>(0.0), INVALID_INDEX, 0u);
        return;
    }

    let ray = rayInput[rayId];
    if !trace_ray_is_valid(ray) {
        return;
    }

    let capacity = two_stage_candidate_capacity();
    let candidateCount = min(blas_candidate_effective_count(blasCandidateCounts[rayId]), capacity);
    if candidateCount == 0u {
        trace_stat_add_miss();
        write_hit_output(rayId, INVALID_INDEX, INVALID_INDEX, 0.0, vec2<f32>(0.0), INVALID_INDEX, 0u);
        return;
    }

    var hitState = HitState(trace_ray_tmax(ray), INVALID_INDEX, INVALID_INDEX, vec2<f32>(0.0), 0u);
    let base = rayId * capacity;
    let testLimit = min(candidateCount, two_stage_blas_test_limit());
    var testedCandidates = 0u;
    for (var i = 0u; i < TLAS_BLAS_CANDIDATE_STACK_SIZE; i += 1u) {
        if i >= testLimit {
            break;
        }

        let candidate = blasCandidates[base + i];
        if candidate.instanceIndex == INVALID_INDEX || candidate.blasHandle == INVALID_INDEX {
            continue;
        }
        if candidate.nearT > hitState.bestT {
            trace_stat_add_two_stage_distance_early_stop();
            break;
        }

        testedCandidates += 1u;
        trace_stat_add_two_stage_blas_candidate_test();
        trace_instance_blas_local(
            trace_ray_origin(ray),
            trace_ray_direction(ray),
            candidate.instanceIndex,
            candidate.nearT,
            vec2<u32>(0u, 0x80000000u),
            &hitState);
    }

    store_trace_result(rayId, hitState);
    hitOutput[rayId]._padding0 = testedCandidates;
}

fn trace_fused_tlas_blas_for_ray(rayId: u32, traceEnabled: bool) {
    if rayId >= uniforms.rayCount {
        return;
    }

    blasCandidateCounts[rayId] = 0u;
    if !traceEnabled {
        write_hit_output(rayId, INVALID_INDEX, INVALID_INDEX, 0.0, vec2<f32>(0.0), INVALID_INDEX, 0u);
        return;
    }

    trace_stat_begin_ray();
    let ray = rayInput[rayId];
    if !trace_ray_is_valid(ray) {
        trace_stat_add_invalid_ray();
        write_hit_output(rayId, INVALID_INDEX, INVALID_INDEX, 0.0, vec2<f32>(0.0), INVALID_INDEX, 0u);
        return;
    }

    trace_stat_begin_valid_ray();
    trace_tlas_blas_candidates_local(trace_ray_origin(ray), trace_ray_direction(ray), trace_ray_tmax(ray), rayId, true);
}

@compute @workgroup_size(WORKGROUP_SIZE_X, WORKGROUP_SIZE_Y, 1)
fn main_trace_blas_candidates(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let traceEnabled = (uniforms.overlayFlags & OVERLAY_FLAG_TRACE_ENABLED) != 0u;
    let queueIndex = globalId.y * uniforms.rayDispatchWidth + globalId.x;
    var rayId = queueIndex;
    if uniforms.traceControl.w > 0.5 && queueIndex < uniforms.rayCount {
        rayId = raySortIds[queueIndex];
    }

    trace_sorted_blas_candidates_for_ray(rayId, traceEnabled);
}

@compute @workgroup_size(WORKGROUP_SIZE_X, WORKGROUP_SIZE_Y, 1)
fn main_trace_tlas_blas_fused(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let traceEnabled = (uniforms.overlayFlags & OVERLAY_FLAG_TRACE_ENABLED) != 0u;
    let queueIndex = globalId.y * uniforms.rayDispatchWidth + globalId.x;
    var rayId = queueIndex;
    if uniforms.traceControl.w > 0.5 && queueIndex < uniforms.rayCount {
        rayId = raySortIds[queueIndex];
    }

    trace_fused_tlas_blas_for_ray(rayId, traceEnabled);
}

fn trace_tlas_any_hit_local(rayOrigin: vec3<f32>, rayDirection: vec3<f32>, maxRayT: f32) -> bool {
    if uniforms.tlasNodeCount == 0u {
        return false;
    }

    let invdir = safe_inverse_direction(rayDirection);
    var octinv = 7u - ((select(0u, 4u, rayDirection.x < 0.0)) | (select(0u, 2u, rayDirection.y < 0.0)) | (select(0u, 1u, rayDirection.z < 0.0)));
    var traversalStack: array<vec4<u32>, TLAS_TRAVERSAL_STACK_SIZE>;
    var stackPtr: u32 = 0u;
    var ngroup = vec2<u32>(0u, 0x80000000u);
    var ngroupMinT = 0.0;
    var tgroup = vec2<u32>(0u, 0u);
    var tgroupMinT = maxRayT;
    let orig = rayOrigin;
    let tmin = uniforms.tMin;
    let tmax = maxRayT;

    loop {
        if (ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) > 0x00FFFFFFu && ngroupMinT > maxRayT {
            ngroup = vec2<u32>(0u, 0u);
        }

        if (ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) > 0x00FFFFFFu {
            let parentUsesSlotOrder = (ngroup.y & TLAS_GROUP_SLOT_ORDER_MASK_FLAG) != 0u;
            let imask = ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG;
            let parentOctinv = select(octinv, 0u, parentUsesSlotOrder);
            let childBitIndex = most_significant_bit_index(imask);
            let childNodeBaseIndex = ngroup.x;

            let remainingMask = imask & ~(1u << childBitIndex);
            ngroup.y = remainingMask | select(0u, TLAS_GROUP_SLOT_ORDER_MASK_FLAG, parentUsesSlotOrder && remainingMask > 0x00FFFFFFu);
            if (ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) > 0x00FFFFFFu {
                if !push_tlas_stack_entry(&traversalStack, &stackPtr, ngroup.x, ngroup.y, ngroupMinT) {
                    return false;
                }
            }

            let slotIndex: u32 = (childBitIndex - 24u) ^ parentOctinv;
            let childNodeIndex: u32 = childNodeBaseIndex + countOneBits(imask & ~(0xFFFFFFFFu << slotIndex));
            let nodeBase = childNodeIndex * 5u;
            let nodeUsesDobb = tlas_dobb_node_active(childNodeIndex);
            let nodeRotationIndex = select(0u, tlasNodeMetadata[childNodeIndex].rotationIndex, nodeUsesDobb);
            let n0 = select(tlasNodes[nodeBase + 0u], tlasDobbNodes[nodeBase + 0u], nodeUsesDobb);
            let n1 = select(tlasNodes[nodeBase + 1u], tlasDobbNodes[nodeBase + 1u], nodeUsesDobb);
            let n2 = select(tlasNodes[nodeBase + 2u], tlasDobbNodes[nodeBase + 2u], nodeUsesDobb);
            let n3 = select(tlasNodes[nodeBase + 3u], tlasDobbNodes[nodeBase + 3u], nodeUsesDobb);
            let n4 = select(tlasNodes[nodeBase + 4u], tlasDobbNodes[nodeBase + 4u], nodeUsesDobb);
            let nodeRayOrigin = select(rayOrigin, tlas_dobb_rotate_vector(rayOrigin, nodeRotationIndex), nodeUsesDobb);
            let nodeRayDirection = select(rayDirection, tlas_dobb_rotate_vector(rayDirection, nodeRotationIndex), nodeUsesDobb);
            let nodeInvdir = safe_inverse_direction(nodeRayDirection);
            let nodeOctinv = select(octinv, 0u, nodeUsesDobb);
            let p = vec3<f32>(bitcast<f32>(n0.x), bitcast<f32>(n0.y), bitcast<f32>(n0.z));
            let e_imask = unpack4xU8(n0.w);
            let octinv4 = nodeOctinv * 0x01010101u;
            let adjusted_idirx: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.x) + 127) << 23u) * nodeInvdir.x;
            let adjusted_idiry: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.y) + 127) << 23u) * nodeInvdir.y;
            let adjusted_idirz: f32 = bitcast<f32>(u32(decode_signed_u8(e_imask.z) + 127) << 23u) * nodeInvdir.z;
            let origx: f32 = -(nodeRayOrigin.x - p.x) * nodeInvdir.x;
            let origy: f32 = -(nodeRayOrigin.y - p.y) * nodeInvdir.y;
            let origz: f32 = -(nodeRayOrigin.z - p.z) * nodeInvdir.z;
            var hitmask = 0u;
            var childHitBits0 = vec4<u32>(0u);
            var childHitBits1 = vec4<u32>(0u);
            var childMinT0 = vec4<f32>(maxRayT);
            var childMinT1 = vec4<f32>(maxRayT);

            {
                let meta4: u32 = n1.z;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.x, n3.z, nodeInvdir.x < 0.0);
                    let swizzledHix = select(n3.z, n2.x, nodeInvdir.x < 0.0);
                    let swizzledLoy = select(n2.z, n4.x, nodeInvdir.y < 0.0);
                    let swizzledHiy = select(n4.x, n2.z, nodeInvdir.y < 0.0);
                    let swizzledLoz = select(n3.x, n4.z, nodeInvdir.z < 0.0);
                    let swizzledHiz = select(n4.z, n3.x, nodeInvdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits0[i] = hitBits;
                                childMinT0[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }
            {
                let meta4: u32 = n1.w;
                let is_inner4: u32 = (meta4 & (meta4 << 1u)) & 0x10101010u;
                let inner_mask4: u32 = sign_extend_s8x4(is_inner4 << 3u);
                let bit_index4: u32 = (meta4 ^ (octinv4 & inner_mask4)) & 0x1F1F1F1Fu;
                let child_bits4: u32 = (meta4 >> 5u) & 0x07070707u;
                if child_bits4 != 0u {
                    let swizzledLox = select(n2.y, n3.w, nodeInvdir.x < 0.0);
                    let swizzledHix = select(n3.w, n2.y, nodeInvdir.x < 0.0);
                    let swizzledLoy = select(n2.w, n4.y, nodeInvdir.y < 0.0);
                    let swizzledHiy = select(n4.y, n2.w, nodeInvdir.y < 0.0);
                    let swizzledLoz = select(n3.y, n4.w, nodeInvdir.z < 0.0);
                    let swizzledHiz = select(n4.w, n3.y, nodeInvdir.z < 0.0);
                    for (var i = 0u; i < 4u; i += 1u) {
                        let childBits = extract_byte(child_bits4, i);
                        if childBits != 0u {
                            let shift = i * 8u;
                            let cmin = max(max(max(
                                f32((swizzledLox >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledLoy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledLoz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmin);
                            let cmax = min(min(min(
                                f32((swizzledHix >> shift) & 0xffu) * adjusted_idirx + origx,
                                f32((swizzledHiy >> shift) & 0xffu) * adjusted_idiry + origy),
                                f32((swizzledHiz >> shift) & 0xffu) * adjusted_idirz + origz),
                                tmax);
                            if cmin <= cmax {
                                let bitIndex = extract_byte(bit_index4, i);
                                let hitBits = childBits << bitIndex;
                                childHitBits1[i] = hitBits;
                                childMinT1[i] = cmin;
                                hitmask |= hitBits;
                            }
                        }
                    }
                }
            }

            var bestBaseIndex = 0u;
            var bestMask = 0u;
            var bestMinT = maxRayT;
            var bestIsInternal = false;
            let internalGroupMask = (hitmask & 0xFF000000u) | e_imask.w;
            let internalPrefixMask = internalGroupMask & 0x00FFFFFFu;
            for (var childSlot = 0u; childSlot < 8u; childSlot += 1u) {
                var hitBits = 0u;
                var childMinT = maxRayT;
                if childSlot < 4u {
                    hitBits = childHitBits0[childSlot];
                    childMinT = childMinT0[childSlot];
                } else {
                    hitBits = childHitBits1[childSlot - 4u];
                    childMinT = childMinT1[childSlot - 4u];
                }
                if hitBits == 0u {
                    continue;
                }
                let childIsInternal = (hitBits & 0xFF000000u) != 0u;
                let baseIndex = select(n1.y, n1.x, childIsInternal);
                let scheduledMaskBase = select(hitBits, hitBits | internalPrefixMask, childIsInternal);
                let scheduledMask = scheduledMaskBase | select(0u, TLAS_GROUP_SLOT_ORDER_MASK_FLAG, nodeUsesDobb && childIsInternal);
                if bestMask == 0u || childMinT < bestMinT {
                    if bestMask != 0u && !push_tlas_stack_entry(&traversalStack, &stackPtr, bestBaseIndex, bestMask, bestMinT) {
                        return false;
                    }
                    bestBaseIndex = baseIndex;
                    bestMask = scheduledMask;
                    bestMinT = childMinT;
                    bestIsInternal = childIsInternal;
                } else if !push_tlas_stack_entry(&traversalStack, &stackPtr, baseIndex, scheduledMask, childMinT) {
                    return false;
                }
            }
            if bestMask == 0u {
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = maxRayT;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = maxRayT;
            } else if bestIsInternal {
                ngroup = vec2<u32>(bestBaseIndex, bestMask);
                ngroupMinT = bestMinT;
                tgroup = vec2<u32>(0u, 0u);
                tgroupMinT = maxRayT;
            } else {
                ngroup = vec2<u32>(0u, 0u);
                ngroupMinT = maxRayT;
                tgroup = vec2<u32>(bestBaseIndex, bestMask);
                tgroupMinT = bestMinT;
            }
        } else {
            tgroup = ngroup;
            tgroupMinT = ngroupMinT;
            ngroup = vec2<u32>(0u, 0u);
        }

        if tgroup.y != 0u && tgroupMinT <= maxRayT {
            while tgroup.y != 0u {
                let leafBit = most_significant_bit_index(tgroup.y);
                let instanceIndex = tlasLeafInstances[tgroup.x + leafBit];
                if instanceIndex != INVALID_INDEX {
                    let leaf = tlasInstanceAabbs[instanceIndex];
                    let worldInterval = intersect_aabb_interval_invdir(rayOrigin, invdir, leaf.minPoint, leaf.maxPoint);
                    let nearT = max(worldInterval.x, uniforms.tMin);
                    let farT = min(worldInterval.y, maxRayT);
                    if leaf.blasHandle != INVALID_INDEX && nearT <= farT {
                        if trace_instance_blas_any_hit_local(rayOrigin, rayDirection, leaf.instanceIndex, nearT, maxRayT) {
                            return true;
                        }
                    }
                }
                tgroup.y &= ~(1u << leafBit);
            }
        }

        if (ngroup.y & ~TLAS_GROUP_SLOT_ORDER_MASK_FLAG) <= 0x00FFFFFFu {
            let entry = pop_nearest_stack_entry(&traversalStack, &stackPtr, maxRayT, maxRayT, false, 0.0);
            if entry.y != 0u {
                ngroup = entry.xy;
                ngroupMinT = bitcast<f32>(entry.z);
            } else {
                return false;
            }
        }
    }

    return false;
}

struct Bvh4StackItem {
    index: u32,
    tBits: u32,
    kind: u32,
    pad: u32,
};

fn select_nearest4(t: vec4<f32>, validMask: u32) -> u32 {
    var bestSlot = INVALID_INDEX;
    var bestT = uniforms.tMax;
    if (((validMask & 1u) != 0u) && (t.x < bestT)) {
        bestT = t.x;
        bestSlot = 0u;
    }
    if (((validMask & 2u) != 0u) && (t.y < bestT)) {
        bestT = t.y;
        bestSlot = 1u;
    }
    if (((validMask & 4u) != 0u) && (t.z < bestT)) {
        bestT = t.z;
        bestSlot = 2u;
    }
    if (((validMask & 8u) != 0u) && (t.w < bestT)) {
        bestSlot = 3u;
    }
    return bestSlot;
}

fn select_farthest4(t: vec4<f32>, validMask: u32) -> u32 {
    var bestSlot = INVALID_INDEX;
    var bestT = -1.0;
    if (((validMask & 1u) != 0u) && (t.x > bestT)) {
        bestT = t.x;
        bestSlot = 0u;
    }
    if (((validMask & 2u) != 0u) && (t.y > bestT)) {
        bestT = t.y;
        bestSlot = 1u;
    }
    if (((validMask & 4u) != 0u) && (t.z > bestT)) {
        bestT = t.z;
        bestSlot = 2u;
    }
    if (((validMask & 8u) != 0u) && (t.w > bestT)) {
        bestSlot = 3u;
    }
    return bestSlot;
}

fn bvh4_stack_item_from_slot(childT: vec4<f32>, childKind: vec4<u32>, childIndex: vec4<u32>, slot: u32) -> Bvh4StackItem {
    switch slot {
        case 0u: { return Bvh4StackItem(childIndex.x, bitcast<u32>(childT.x), childKind.x, 0u); }
        case 1u: { return Bvh4StackItem(childIndex.y, bitcast<u32>(childT.y), childKind.y, 0u); }
        case 2u: { return Bvh4StackItem(childIndex.z, bitcast<u32>(childT.z), childKind.z, 0u); }
        default: { return Bvh4StackItem(childIndex.w, bitcast<u32>(childT.w), childKind.w, 0u); }
    }
}

fn push_bvh4_stack_entry(
    traversalStack: ptr<function, array<Bvh4StackItem, TLAS_TRAVERSAL_STACK_SIZE>>,
    stackPtr: ptr<function, u32>,
    item: Bvh4StackItem) -> bool {
    if (*stackPtr) >= TLAS_TRAVERSAL_STACK_SIZE {
        trace_stat_add_local_stack_spill(1u);
        return false;
    }
    (*traversalStack)[(*stackPtr)] = item;
    (*stackPtr) += 1u;
    return true;
}

fn pop_nearest_bvh4_stack_entry(
    traversalStack: ptr<function, array<Bvh4StackItem, TLAS_TRAVERSAL_STACK_SIZE>>,
    stackPtr: ptr<function, u32>,
    bestT: f32,
    earlyOut: bool,
    earlyOutBias: f32) -> Bvh4StackItem {
    var bestSlot = INVALID_INDEX;
    var bestMinT = uniforms.tMax;
    for (var slot = 0u; slot < TLAS_TRAVERSAL_STACK_SIZE; slot += 1u) {
        if slot >= (*stackPtr) {
            break;
        }
        let entry = (*traversalStack)[slot];
        let entryMinT = bitcast<f32>(entry.tBits);
        if entry.kind == 0u || should_skip_group_fast(entryMinT, bestT, earlyOut, earlyOutBias) {
            continue;
        }
        if bestSlot == INVALID_INDEX || entryMinT < bestMinT {
            bestSlot = slot;
            bestMinT = entryMinT;
        }
    }

    if bestSlot == INVALID_INDEX {
        (*stackPtr) = 0u;
        return Bvh4StackItem(0u, bitcast<u32>(uniforms.tMax), 0u, 0u);
    }

    let entry = (*traversalStack)[bestSlot];
    (*stackPtr) -= 1u;
    (*traversalStack)[bestSlot] = (*traversalStack)[(*stackPtr)];
    return entry;
}

fn trace_tlas_bvh4_local(rayOrigin: vec3<f32>, rayDirection: vec3<f32>, state: ptr<function, HitState>) {
    if uniforms.tlasNodeCount == 0u {
        return;
    }

    let invdir = safe_inverse_direction(rayDirection);
    var traversalStack: array<Bvh4StackItem, TLAS_TRAVERSAL_STACK_SIZE>;
    var stackPtr = 0u;
    var current = Bvh4StackItem(0u, bitcast<u32>(0.0), 2u, 0u);
    let earlyOut = traversal_early_out_enabled();
    let earlyOutBias = max(uniforms.traceControl.y, 0.0);

    loop {
        let currentT = bitcast<f32>(current.tBits);
        if should_skip_group_fast(currentT, (*state).bestT, earlyOut, earlyOutBias) {
            current.kind = 0u;
        }

        if current.kind == 1u {
            trace_instance_blas_local(rayOrigin, rayDirection, current.index, currentT, vec2<u32>(0u, 0x80000000u), state);
            current.kind = 0u;
        } else if current.kind == 2u {
            (*state).iterationCount += 1u;
            let node = tlasBvh4Nodes[current.index];
            let tx0 = (node.childMinX - vec4<f32>(rayOrigin.x)) * vec4<f32>(invdir.x);
            let tx1 = (node.childMaxX - vec4<f32>(rayOrigin.x)) * vec4<f32>(invdir.x);
            let ty0 = (node.childMinY - vec4<f32>(rayOrigin.y)) * vec4<f32>(invdir.y);
            let ty1 = (node.childMaxY - vec4<f32>(rayOrigin.y)) * vec4<f32>(invdir.y);
            let tz0 = (node.childMinZ - vec4<f32>(rayOrigin.z)) * vec4<f32>(invdir.z);
            let tz1 = (node.childMaxZ - vec4<f32>(rayOrigin.z)) * vec4<f32>(invdir.z);
            let tNear = max(max(min(tx0, tx1), min(ty0, ty1)), min(tz0, tz1));
            let tFar = min(min(max(tx0, tx1), max(ty0, ty1)), max(tz0, tz1));
            let nearT = max(tNear, vec4<f32>(uniforms.tMin));
            let farT = min(tFar, vec4<f32>(min((*state).bestT, uniforms.tMax)));

            var childT = nearT;
            var childKind = node.childMeta;
            var childIndex = vec4<u32>(INVALID_INDEX);
            var validMask = 0u;
            if ((node.childMeta.x != 0u) && (nearT.x <= farT.x)) {
                childIndex.x = node.childIndex.x;
                validMask |= 1u;
            }
            if ((node.childMeta.y != 0u) && (nearT.y <= farT.y)) {
                childIndex.y = node.childIndex.y;
                validMask |= 2u;
            }
            if ((node.childMeta.z != 0u) && (nearT.z <= farT.z)) {
                childIndex.z = node.childIndex.z;
                validMask |= 4u;
            }
            if ((node.childMeta.w != 0u) && (nearT.w <= farT.w)) {
                childIndex.w = node.childIndex.w;
                validMask |= 8u;
            }

            let nearestSlot = select_nearest4(childT, validMask);
            current.kind = 0u;

            var pushMask = validMask;
            if nearestSlot != INVALID_INDEX {
                pushMask &= ~(1u << nearestSlot);
            }
            for (var pushOrder = 0u; pushOrder < 3u; pushOrder += 1u) {
                let farthestSlot = select_farthest4(childT, pushMask);
                if farthestSlot == INVALID_INDEX {
                    break;
                }
                if !push_bvh4_stack_entry(&traversalStack, &stackPtr, bvh4_stack_item_from_slot(childT, childKind, childIndex, farthestSlot)) {
                    return;
                }
                pushMask &= ~(1u << farthestSlot);
            }

            if nearestSlot != INVALID_INDEX {
                current = bvh4_stack_item_from_slot(childT, childKind, childIndex, nearestSlot);
            }
        }

        if current.kind == 0u {
            current = pop_nearest_bvh4_stack_entry(&traversalStack, &stackPtr, (*state).bestT, earlyOut, earlyOutBias);
            if current.kind == 0u {
                return;
            }
        }
    }
}
#endif

fn trace_ray(rayId: u32) {
    if rayId >= uniforms.rayCount {
        return;
    }

    trace_stat_begin_ray();
    let ray = rayInput[rayId];
    if !trace_ray_is_valid(ray) {
        trace_stat_add_invalid_ray();
        if debug_mode_heatmap() {
            write_hit_output(rayId, DEBUG_HIT_INSTANCE_INVALID_RAY, 0u, 0.0, vec2<f32>(0.0), INVALID_INDEX, 0u);
        } else {
            write_hit_output(rayId, INVALID_INDEX, INVALID_INDEX, 0.0, vec2<f32>(0.0), INVALID_INDEX, 0u);
        }
        return;
    }

    trace_stat_begin_valid_ray();
    if occlusion_trace_enabled() {
#if !defined(SWRT_WORLD_CWBVH) && !defined(SWRT_TLAS_BVH4)
        if trace_tlas_any_hit_local(trace_ray_origin(ray), trace_ray_direction(ray), trace_ray_tmax(ray)) {
            trace_stat_add_hit(0u, 0u);
            write_hit_output(rayId, 0u, 0u, trace_ray_tmax(ray), vec2<f32>(0.0), 0u, 1u);
        } else {
            trace_stat_add_miss();
            write_hit_output(rayId, INVALID_INDEX, INVALID_INDEX, 0.0, vec2<f32>(0.0), INVALID_INDEX, 0u);
        }
        return;
#endif
    }
    var hitState = HitState(trace_ray_tmax(ray), INVALID_INDEX, INVALID_INDEX, vec2<f32>(0.0), 0u);
#ifdef SWRT_WORLD_CWBVH
    trace_world_cwbvh(trace_ray_origin(ray), trace_ray_direction(ray), &hitState);
#elif defined(SWRT_TLAS_BVH4)
    trace_tlas_bvh4_local(trace_ray_origin(ray), trace_ray_direction(ray), &hitState);
#else
    trace_tlas_local(trace_ray_origin(ray), trace_ray_direction(ray), &hitState);
#endif
    store_trace_result(rayId, hitState);
}
