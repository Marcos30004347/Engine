const SPARSE_VOXEL_SCENE_RAY_STATUS_INVALID : u32 = 0u;
const SPARSE_VOXEL_SCENE_RAY_STATUS_PENDING : u32 = 1u;
const SPARSE_VOXEL_SCENE_RAY_STATUS_HIT : u32 = 2u;
const SPARSE_VOXEL_SCENE_RAY_STATUS_MISS_TMAX : u32 = 3u;
const SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_SCENE : u32 = 4u;
const SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_TREE_VOLUME : u32 = 5u;
const SPARSE_VOXEL_SCENE_RAY_STATUS_UNKNOWN_RESIDENCY : u32 = 6u;
const SPARSE_VOXEL_SCENE_RAY_STATUS_QUERY_LIMIT : u32 = 7u;

const SPARSE_VOXEL_SCENE_RAY_BACKEND_NONE : u32 = 0u;
const SPARSE_VOXEL_SCENE_RAY_BACKEND_SCREEN : u32 = 1u;
const SPARSE_VOXEL_SCENE_RAY_BACKEND_TREE64 : u32 = 2u;

const SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE : u32 = 1u << 0u;
const SPARSE_VOXEL_SCENE_RAY_FLAG_SHADOW : u32 = 1u << 1u;
const SPARSE_VOXEL_SCENE_RAY_FLAG_REGION_SENTINEL : u32 = 1u << 2u;
// Probe rays bypass screen tracing and therefore need their conservative
// minimum distance applied by the world-space compaction pass.
const SPARSE_VOXEL_SCENE_RAY_FLAG_PROBE : u32 = 1u << 3u;
// Six axis-aligned receiver faces are carried in otherwise-unused flag bits.
// Tree64 uses the face to identify and skip the authored receiver cell while
// leaving the reconstructed traversal origin unchanged. Moving even one
// world-f32 ULP is a centimetre-scale shadow bias at planet coordinates.
const SPARSE_VOXEL_SCENE_RAY_RECEIVER_FACE_SHIFT : u32 = 4u;
const SPARSE_VOXEL_SCENE_RAY_RECEIVER_FACE_MASK : u32 =
    7u << SPARSE_VOXEL_SCENE_RAY_RECEIVER_FACE_SHIFT;

fn sparse_voxel_receiver_face(normal : vec3<f32>) -> u32 {
    let absoluteNormal = abs(normal);
    var axis = 0u;
    if (absoluteNormal.y > absoluteNormal.x) { axis = 1u; }
    if (absoluteNormal.z > absoluteNormal[axis]) { axis = 2u; }
    let positive = normal[axis] >= 0.0;
    // Zero is reserved for rays (such as probes) with no raster receiver.
    return 1u + axis * 2u + select(1u, 0u, positive);
}

fn sparse_voxel_receiver_face_flags(normal : vec3<f32>) -> u32 {
    return sparse_voxel_receiver_face(normal) <<
        SPARSE_VOXEL_SCENE_RAY_RECEIVER_FACE_SHIFT;
}

fn sparse_voxel_copy_receiver_face(
    destinationFlags : u32, sourceFlags : u32) -> u32 {
    return (destinationFlags &
            ~SPARSE_VOXEL_SCENE_RAY_RECEIVER_FACE_MASK) |
        (sourceFlags & SPARSE_VOXEL_SCENE_RAY_RECEIVER_FACE_MASK);
}

const SPARSE_VOXEL_REGION_STABILITY_HEADER_WORDS : u32 = 4u;
const SPARSE_VOXEL_REGION_STABILITY_LIGHT_CAPACITY : u32 = 8u;
const SPARSE_VOXEL_REGION_STABILITY_WORDS_PER_TILE : u32 =
    SPARSE_VOXEL_REGION_STABILITY_HEADER_WORDS +
    SPARSE_VOXEL_REGION_STABILITY_LIGHT_CAPACITY;

// Adaptive tile ray budgeting. Mirrors the constants in SparseVoxelGPU.hpp.
// Two statistics banks selected by frame parity, followed by one record per
// screen tile.
const SPARSE_VOXEL_RAY_BUDGET_BANK_WORDS : u32 = 16u;
const SPARSE_VOXEL_RAY_BUDGET_HEADER_WORDS : u32 = 32u;
const SPARSE_VOXEL_RAY_BUDGET_WORDS_PER_TILE : u32 = 10u;
const SPARSE_VOXEL_RAY_BUDGET_DEMAND_SCALE : f32 = 256.0;
// Mean per-pixel demand above which a tile is reported as awake. Purely a
// statistics threshold: the quota itself is continuous in the tile's demand.
const SPARSE_VOXEL_RAY_BUDGET_AWAKE_DEMAND : f32 = 0.125;

const SPARSE_VOXEL_RAY_BUDGET_STAT_FRAME : u32 = 0u;
const SPARSE_VOXEL_RAY_BUDGET_STAT_COVERED_TILES : u32 = 1u;
const SPARSE_VOXEL_RAY_BUDGET_STAT_TOTAL_DEMAND : u32 = 2u;
const SPARSE_VOXEL_RAY_BUDGET_STAT_TILES_AWAKE : u32 = 3u;
const SPARSE_VOXEL_RAY_BUDGET_STAT_TILES_CONVERGED : u32 = 4u;
const SPARSE_VOXEL_RAY_BUDGET_STAT_TOTAL_BUDGET : u32 = 5u;
const SPARSE_VOXEL_RAY_BUDGET_STAT_RAYS_SCHEDULED : u32 = 6u;
const SPARSE_VOXEL_RAY_BUDGET_STAT_HEARTBEAT_RAYS : u32 = 7u;
const SPARSE_VOXEL_RAY_BUDGET_STAT_DISOCCLUSION_RAYS : u32 = 8u;
const SPARSE_VOXEL_RAY_BUDGET_STAT_MOTION_RAYS : u32 = 9u;
const SPARSE_VOXEL_RAY_BUDGET_STAT_LIGHTING_RAYS : u32 = 10u;
const SPARSE_VOXEL_RAY_BUDGET_STAT_NOISE_RAYS : u32 = 11u;
const SPARSE_VOXEL_RAY_BUDGET_STAT_UNSTABLE_RAYS : u32 = 12u;
const SPARSE_VOXEL_RAY_BUDGET_STAT_RESOLVED_RAYS : u32 = 13u;
const SPARSE_VOXEL_RAY_BUDGET_STAT_VISIBILITY_CHANGES : u32 = 14u;
// Total covered receivers. Demand is redistributed per pixel, not per tile:
// a partially covered edge tile has a small absolute demand sum but can be as
// urgent as a full one, and dividing by tiles would let every fully covered
// tile saturate the boost.
const SPARSE_VOXEL_RAY_BUDGET_STAT_COVERED_PIXELS : u32 = 15u;

// Per-tile record words.
const SPARSE_VOXEL_RAY_BUDGET_TILE_COVERED : u32 = 0u;
const SPARSE_VOXEL_RAY_BUDGET_TILE_DISOCCLUDED : u32 = 1u;
const SPARSE_VOXEL_RAY_BUDGET_TILE_MOVING : u32 = 2u;
const SPARSE_VOXEL_RAY_BUDGET_TILE_NOISY : u32 = 3u;
const SPARSE_VOXEL_RAY_BUDGET_TILE_DEMAND : u32 = 4u;
// Low 24 bits maximum demand in fixed point, high eight bits the wake reason.
const SPARSE_VOXEL_RAY_BUDGET_TILE_CONFIDENCE : u32 = 5u;
const SPARSE_VOXEL_RAY_BUDGET_TILE_QUOTA : u32 = 6u;
const SPARSE_VOXEL_RAY_BUDGET_TILE_SCHEDULED : u32 = 7u;
// Accumulated by the light-field resolve, cleared by the next scheduler pass.
const SPARSE_VOXEL_RAY_BUDGET_TILE_RESOLVED : u32 = 8u;
const SPARSE_VOXEL_RAY_BUDGET_TILE_VISIBILITY_CHANGES : u32 = 9u;

const SPARSE_VOXEL_RAY_BUDGET_WAKE_DISOCCLUSION : u32 = 1u << 0u;
const SPARSE_VOXEL_RAY_BUDGET_WAKE_MOTION : u32 = 1u << 1u;
const SPARSE_VOXEL_RAY_BUDGET_WAKE_LIGHTING : u32 = 1u << 2u;
const SPARSE_VOXEL_RAY_BUDGET_WAKE_NOISE : u32 = 1u << 3u;
const SPARSE_VOXEL_RAY_BUDGET_WAKE_UNCONVERGED : u32 = 1u << 4u;
const SPARSE_VOXEL_RAY_BUDGET_WAKE_HEARTBEAT : u32 = 1u << 5u;

// Demand priority classes, highest first. The class is the high half of the
// selection key so the ordering below is respected exactly; the quantised
// demand inside a class only breaks ties.
const SPARSE_VOXEL_RAY_BUDGET_CLASS_NONE : u32 = 0u;
const SPARSE_VOXEL_RAY_BUDGET_CLASS_HEARTBEAT : u32 = 1u;
const SPARSE_VOXEL_RAY_BUDGET_CLASS_UNCONVERGED : u32 = 2u;
const SPARSE_VOXEL_RAY_BUDGET_CLASS_NOISE : u32 = 3u;
const SPARSE_VOXEL_RAY_BUDGET_CLASS_LIGHTING : u32 = 4u;
const SPARSE_VOXEL_RAY_BUDGET_CLASS_MOTION : u32 = 5u;
const SPARSE_VOXEL_RAY_BUDGET_CLASS_DISOCCLUSION : u32 = 6u;
const SPARSE_VOXEL_RAY_BUDGET_CLASS_COUNT : u32 = 7u;
// Eight quantised demand steps per class gives a 56 entry histogram; the
// selection scan below is padded to 64.
const SPARSE_VOXEL_RAY_BUDGET_DEMAND_STEPS : u32 = 8u;
const SPARSE_VOXEL_RAY_BUDGET_BUCKETS : u32 = 64u;

struct SparseVoxelSceneRay {
    originMinimumDistance : vec4<f32>,
    directionMaximumDistance : vec4<f32>,
    pixelRayLight : vec4<u32>,
    // seed, flags, scene generation, world-region reachability key.
    seedFlags : vec4<u32>,
    lightSamplePdfWeight : vec4<f32>,
}

struct SparseVoxelSceneRayHit {
    hitPositionDistance : vec4<f32>,
    globalCell : vec3<i32>,
    status : u32,
    identity : vec4<u32>,
}

// Per-lane ReSTIR DI state. Selected rays remain in a dense
// pixel-major/ray-lane buffer so tracing backends need no reservoir-specific
// ABI.
struct SparseVoxelDIReservoir {
    identity : vec4<u32>,
    weights : vec4<f32>,
    state : vec4<u32>,
    receiverNormalDepth : vec4<f32>,
    receiverIdentity : vec4<u32>,
}

fn sparse_voxel_pack_octahedral_normal(normal : vec3<f32>) -> u32 {
    let normalLength = length(normal);
    let n = select(
        vec3<f32>(0.0, 1.0, 0.0),
        normal / max(normalLength, 1.0e-6), normalLength > 1.0e-6);
    var oct = n.xy / max(abs(n.x) + abs(n.y) + abs(n.z), 1.0e-6);
    if (n.z < 0.0) {
        oct = (vec2<f32>(1.0) - abs(oct.yx)) * vec2<f32>(
            select(-1.0, 1.0, oct.x >= 0.0),
            select(-1.0, 1.0, oct.y >= 0.0));
    }
    let quantized = vec2<u32>(round(clamp(
        oct * 0.5 + vec2<f32>(0.5), vec2<f32>(0.0), vec2<f32>(1.0)) *
        65535.0));
    return quantized.x | (quantized.y << 16u);
}

fn sparse_voxel_unpack_octahedral_normal(packed : u32) -> vec3<f32> {
    let oct = vec2<f32>(
        f32(packed & 0xffffu), f32(packed >> 16u)) /
        65535.0 * 2.0 - vec2<f32>(1.0);
    var normal = vec3<f32>(oct, 1.0 - abs(oct.x) - abs(oct.y));
    if (normal.z < 0.0) {
        normal.x = (1.0 - abs(oct.y)) *
            select(-1.0, 1.0, oct.x >= 0.0);
        normal.y = (1.0 - abs(oct.x)) *
            select(-1.0, 1.0, oct.y >= 0.0);
    }
    let normalLength = length(normal);
    return select(
        vec3<f32>(0.0, 1.0, 0.0),
        normal / max(normalLength, 1.0e-6), normalLength > 1.0e-6);
}

fn sparse_voxel_invalid_scene_hit(
    ray : SparseVoxelSceneRay,
    status : u32,
    backend : u32) -> SparseVoxelSceneRayHit {
    return SparseVoxelSceneRayHit(
        vec4<f32>(0.0),
        vec3<i32>(-2147483648),
        status,
        vec4<u32>(ray.seedFlags.x, ray.pixelRayLight.w, backend, ray.seedFlags.z));
}

// A non-hit that still reports how far along the ray the backend got before it
// gave up. The screen tracer uses this so the world tracer can resume from the
// point the ray left the screen instead of re-marching the span that was
// already tested against the depth buffer.
fn sparse_voxel_unresolved_scene_hit_at(
    ray : SparseVoxelSceneRay,
    status : u32,
    backend : u32,
    distance : f32) -> SparseVoxelSceneRayHit {
    return SparseVoxelSceneRayHit(
        vec4<f32>(0.0, 0.0, 0.0, max(distance, 0.0)),
        vec3<i32>(-2147483648),
        status,
        vec4<u32>(ray.seedFlags.x, ray.pixelRayLight.w, backend, ray.seedFlags.z));
}
