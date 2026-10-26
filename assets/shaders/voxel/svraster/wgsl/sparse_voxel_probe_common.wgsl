// Screen-space probe layout shared by placement, ray generation, sharing,
// resolve and interpolation. Mirrors src/voxel/svraster/SparseVoxelProbe.hpp.

const SPARSE_VOXEL_PROBE_OCTAHEDRAL_SIZE : u32 = 8u;
const SPARSE_VOXEL_PROBE_DIRECTION_COUNT : u32 = 64u;

// Solid angle one direction cell subtends. The direction set tiles the
// *hemisphere* above the probe's normal, so the cell measure is 2pi/N, not
// 4pi/N. Getting this constant wrong scales every probe's irradiance by two.
const SPARSE_VOXEL_PROBE_CELL_SOLID_ANGLE : f32 = 6.28318530718 / 64.0;

// Distance written into a radiance cell whose ray reached open sky. Reprojection
// treats a cell at this distance as being at infinity, so its direction is
// carried over unchanged rather than parallax corrected. The value is also the
// clamp the sharing pass quantises against, so it has to stay well inside the
// fixed-point range that pass uses.
const SPARSE_VOXEL_PROBE_SKY_DISTANCE : f32 = 1.0e6;
// A radiance cell with a negative distance carries no sample.
const SPARSE_VOXEL_PROBE_NO_SAMPLE : f32 = -1.0;

const SPARSE_VOXEL_PROBE_STATE_VALID : u32 = 1u << 0u;
const SPARSE_VOXEL_PROBE_STATE_REPROJECTED : u32 = 1u << 1u;
// Placed by adaptive refinement rather than by owning a tile. Only placement
// and interpolation care: to every pass in between, a refined probe is an
// ordinary probe that happens to live past the end of the uniform grid.
const SPARSE_VOXEL_PROBE_STATE_ADAPTIVE : u32 = 1u << 2u;
const SPARSE_VOXEL_PROBE_STATE_DENSITY_SHIFT : u32 = 3u;
const SPARSE_VOXEL_PROBE_STATE_DENSITY_MASK : u32 = 3u << 3u;

fn sparse_voxel_probe_density_stride(state : u32) -> u32 {
    let level = (state & SPARSE_VOXEL_PROBE_STATE_DENSITY_MASK) >>
        SPARSE_VOXEL_PROBE_STATE_DENSITY_SHIFT;
    return 1u << min(level, 2u);
}

fn sparse_voxel_probe_density_state(stride : u32) -> u32 {
    let level = select(0u, select(1u, 2u, stride >= 4u), stride >= 2u);
    return level << SPARSE_VOXEL_PROBE_STATE_DENSITY_SHIFT;
}

fn sparse_voxel_probe_distance_stride(
    depth : f32, halfDistance : f32, quarterDistance : f32,
    maximumStride : u32) -> u32 {
    if (maximumStride >= 4u && depth >= quarterDistance) { return 4u; }
    if (maximumStride >= 2u && depth >= halfDistance) { return 2u; }
    return 1u;
}

// Refined probes one tile may spawn, and the stride of the per-tile registry
// that records them (one count word followed by that many indices). One primary
// plus two secondary probes covers the three visible faces of a voxel cube.
const SPARSE_VOXEL_PROBE_ADAPTIVE_PER_TILE : u32 = 2u;
// Refinement levels below the tile: quadrants, sixteenths, then single pixels
// for an 8x8 tile. Four levels of depth counting the tile itself, which is as
// far as subdivision can go before a region stops containing a surface at all.
//
// The descent is demand driven. A region that the probes above it already
// represent is never subdivided, so a flat tile costs one test per level and
// stops; only a region that still straddles surfaces pays to go deeper.
const SPARSE_VOXEL_PROBE_ADAPTIVE_LEVELS : u32 = 3u;
// Regions at the deepest level, which sizes the placement scratch.
const SPARSE_VOXEL_PROBE_ADAPTIVE_REGIONS : u32 = 64u;

// Budget arbitration bins candidates by how badly their region is
// misrepresented, so a global budget can be spent worst-first instead of
// first-come. Eight bins on a log2 scale spans everything from "fails by a
// hair" to "completely different surface".
const SPARSE_VOXEL_PROBE_PRIORITY_BINS : u32 = 8u;

// Allocation buffer layout. One counter, then this frame's demand histogram,
// then the cutoff the reset stage derived from the frame before.
const SPARSE_VOXEL_PROBE_ALLOCATION_COUNT : u32 = 0u;
const SPARSE_VOXEL_PROBE_ALLOCATION_HISTOGRAM : u32 = 1u;
const SPARSE_VOXEL_PROBE_ALLOCATION_CUTOFF_BIN : u32 = 9u;
const SPARSE_VOXEL_PROBE_ALLOCATION_CUTOFF_SHARE : u32 = 10u;
const SPARSE_VOXEL_PROBE_ALLOCATION_WORDS : u32 = 12u;
// Acceptance share inside the marginal bin, in parts per thousand.
const SPARSE_VOXEL_PROBE_ALLOCATION_SHARE_SCALE : u32 = 1000u;

// Which priority bin a refinement candidate falls in.
//
// The key is the error *times the number of pixels it would fix*, not the error
// alone. A slot buys one probe wherever it is spent, so the budget should go
// where it removes the most error - and a quadrant that is moderately wrong
// across sixteen pixels is worth more than a single pixel that is wildly wrong.
// Ranking on error alone lets the deepest, smallest regions outbid the shallow
// ones that would have covered them, which spends the whole budget on corners.
//
// The error is clamped before combining because the hard rejections - wrong
// orientation, incompatible depth - return a sentinel large enough to saturate
// every bin on its own and erase the size term.
fn sparse_voxel_probe_priority_bin(error : f32, pixels : u32) -> u32 {
    let weighted = min(max(error, 1.0), 64.0) * f32(max(pixels, 1u));
    return min(
        u32(log2(max(weighted, 1.0))), SPARSE_VOXEL_PROBE_PRIORITY_BINS - 1u);
}
const SPARSE_VOXEL_PROBE_ADAPTIVE_TILE_STRIDE : u32 =
    SPARSE_VOXEL_PROBE_ADAPTIVE_PER_TILE + 1u;

// Longest run of frames a probe's directional-cell history may represent.
// Sparse sleeping probes revisit one cell only every several frames, so a cap
// of 64 left them with only eight effective observations and a permanent 1/9
// update weight. Keep enough age for static sky visibility to truly converge.
const SPARSE_VOXEL_PROBE_MAXIMUM_SAMPLES : u32 = 4096u;
// Direct emitters retain a shorter window so a moving shadow caster can still
// change their aggregate even when the receiver itself has not moved.
const SPARSE_VOXEL_PROBE_EVENT_MAXIMUM_SAMPLES : u32 = 128u;

struct SparseVoxelProbe {
    positionDepth : vec4<f32>,
    normalRadius : vec4<f32>,
    identityState : vec4<u32>,
    pixelFrameHistory : vec4<u32>,
}

// Second-order real spherical harmonics, matching Brixelizer GI's probe
// representation (FFX_BRIXELIZER_GI_SH_C*). Nine coefficients per colour
// channel is enough for the diffuse, low-frequency signal soft shadows live in,
// and it is what makes the full-resolution resolve cheap: one dot product per
// pixel instead of interpolating four octahedral maps.
//
// The octahedral radiance map is kept as well, but not for the resolve: it is
// what makes ray sharing possible. A cell holds the radiance *and the hit
// distance* of one direction, which is exactly the information a neighbouring
// probe needs to reproject that sample into its own frame.
const SPARSE_VOXEL_PROBE_SH_COUNT : u32 = 9u;
const SPARSE_VOXEL_PROBE_SH_C0 : f32 = 0.2820947917738781;
const SPARSE_VOXEL_PROBE_SH_C1 : f32 = 0.4886025119029199;
const SPARSE_VOXEL_PROBE_SH_C4 : f32 = 1.0925484305920792;
const SPARSE_VOXEL_PROBE_SH_C5 : f32 = 0.31539156525252005;
const SPARSE_VOXEL_PROBE_SH_C6 : f32 = 0.5462742152960396;

fn sparse_voxel_probe_sh_basis(direction : vec3<f32>) -> array<f32, 9> {
    let d = direction;
    return array<f32, 9>(
        SPARSE_VOXEL_PROBE_SH_C0,
        SPARSE_VOXEL_PROBE_SH_C1 * d.y,
        SPARSE_VOXEL_PROBE_SH_C1 * d.z,
        SPARSE_VOXEL_PROBE_SH_C1 * d.x,
        SPARSE_VOXEL_PROBE_SH_C4 * d.x * d.y,
        SPARSE_VOXEL_PROBE_SH_C4 * d.y * d.z,
        SPARSE_VOXEL_PROBE_SH_C5 * (3.0 * d.z * d.z - 1.0),
        SPARSE_VOXEL_PROBE_SH_C4 * d.x * d.z,
        SPARSE_VOXEL_PROBE_SH_C6 * (d.x * d.x - d.y * d.y));
}

// Convolution of the SH radiance with a clamped cosine lobe, which turns the
// stored radiance into irradiance for a surface facing `normal`. The band
// factors are the standard pi, 2pi/3, pi/4 Lambertian coefficients.
fn sparse_voxel_probe_sh_irradiance(
    coefficients : array<vec4<f32>, 9>, normal : vec3<f32>) -> vec3<f32> {
    let basis = sparse_voxel_probe_sh_basis(normal);
    let bandZero = 3.14159265359;
    let bandOne = 2.0943951023;
    let bandTwo = 0.7853981634;
    var result = coefficients[0].xyz * basis[0] * bandZero;
    result += coefficients[1].xyz * basis[1] * bandOne;
    result += coefficients[2].xyz * basis[2] * bandOne;
    result += coefficients[3].xyz * basis[3] * bandOne;
    result += coefficients[4].xyz * basis[4] * bandTwo;
    result += coefficients[5].xyz * basis[5] * bandTwo;
    result += coefficients[6].xyz * basis[6] * bandTwo;
    result += coefficients[7].xyz * basis[7] * bandTwo;
    result += coefficients[8].xyz * basis[8] * bandTwo;
    return max(result, vec3<f32>(0.0));
}

// The same evaluation with the directional bands damped toward the DC term.
//
// Voxel faces are axis aligned, so where geometry is smaller than a pixel the
// resolved normal flips between perpendicular axis directions from frame to
// frame. Bands one and two are exactly the part of the irradiance that depends
// on which way the normal ended up pointing, so a flip swings the result and the
// pixel visibly switches brightness - "tiny spots that keep changing".
//
// `sharpness` of 1 is the full directional evaluation, 0 is the flat DC term,
// which is the average over the hemisphere and therefore invariant to the flip.
// This is the cheap version of smoothing the normal: it needs no neighbourhood
// filter and no extra coefficients, because damping the bands that carry the
// direction is equivalent to widening the lobe the normal is sampled through.
fn sparse_voxel_probe_sh_irradiance_damped(
    coefficients : array<vec4<f32>, 9>, normal : vec3<f32>,
    sharpness : f32) -> vec3<f32> {
    let basis = sparse_voxel_probe_sh_basis(normal);
    let bandZero = 3.14159265359;
    // Band two carries twice the angular frequency of band one, so it has to
    // fade twice as fast or the quadratic lobe outlives the linear one and the
    // damped result is not a widening lobe at all.
    let weightOne = clamp(sharpness, 0.0, 1.0);
    let weightTwo = weightOne * weightOne;
    let bandOne = 2.0943951023 * weightOne;
    let bandTwo = 0.7853981634 * weightTwo;
    var result = coefficients[0].xyz * basis[0] * bandZero;
    result += coefficients[1].xyz * basis[1] * bandOne;
    result += coefficients[2].xyz * basis[2] * bandOne;
    result += coefficients[3].xyz * basis[3] * bandOne;
    result += coefficients[4].xyz * basis[4] * bandTwo;
    result += coefficients[5].xyz * basis[5] * bandTwo;
    result += coefficients[6].xyz * basis[6] * bandTwo;
    result += coefficients[7].xyz * basis[7] * bandTwo;
    result += coefficients[8].xyz * basis[8] * bandTwo;
    return max(result, vec3<f32>(0.0));
}

fn sparse_voxel_probe_counts(
    dimensions : vec2<u32>, tileSize : u32) -> vec2<u32> {
    let size = max(tileSize, 1u);
    return (max(dimensions, vec2<u32>(1u)) + vec2<u32>(size - 1u)) / size;
}

fn sparse_voxel_probe_index(
    probeCoord : vec2<u32>, probeCounts : vec2<u32>) -> u32 {
    return probeCoord.y * probeCounts.x + probeCoord.x;
}

fn sparse_voxel_probe_normalize(
    value : vec3<f32>, fallback : vec3<f32>) -> vec3<f32> {
    let length2 = dot(value, value);
    return select(
        fallback, value * inverseSqrt(max(length2, 1.0e-12)),
        length2 > 1.0e-12);
}

// Orthonormal frame with the probe normal as its third axis. Duff's branchless
// construction: it is continuous in the normal, which matters here because two
// consecutive frames must agree on a probe's cell layout for reprojected
// samples to land in the cell they belong to.
fn sparse_voxel_probe_basis(normal : vec3<f32>) -> mat3x3<f32> {
    let sign = select(-1.0, 1.0, normal.z >= 0.0);
    let a = -1.0 / (sign + normal.z);
    let b = normal.x * normal.y * a;
    let tangent = vec3<f32>(
        1.0 + sign * normal.x * normal.x * a, sign * b, -sign * normal.x);
    let bitangent = vec3<f32>(b, sign + normal.y * normal.y * a, -normal.y);
    return mat3x3<f32>(tangent, bitangent, normal);
}

// Hemi-octahedral mapping, the same one Brixelizer GI uses for its probes. The
// whole unit square maps to the upper hemisphere, so all 64 cells are usable
// directions above the surface. A full-sphere octahedral map wastes half its
// cells on directions that can only ever return the receiver itself, and
// folding those back with a reflection destroys the stratification the
// estimator's constant cell measure depends on.
fn sparse_voxel_probe_hemioct_decode(uv : vec2<f32>) -> vec3<f32> {
    let e = uv * 2.0 - vec2<f32>(1.0);
    let temp = vec2<f32>(e.x + e.y, e.x - e.y) * 0.5;
    let direction = vec3<f32>(temp, 1.0 - abs(temp.x) - abs(temp.y));
    return sparse_voxel_probe_normalize(direction, vec3<f32>(0.0, 0.0, 1.0));
}

fn sparse_voxel_probe_hemioct_encode(direction : vec3<f32>) -> vec2<f32> {
    let p = direction.xy /
        max(abs(direction.x) + abs(direction.y) + direction.z, 1.0e-8);
    return vec2<f32>(p.x + p.y, p.x - p.y) * 0.5 + vec2<f32>(0.5);
}

fn sparse_voxel_probe_cell_coord(cellIndex : u32) -> vec2<f32> {
    return vec2<f32>(
        f32(cellIndex % SPARSE_VOXEL_PROBE_OCTAHEDRAL_SIZE),
        f32(cellIndex / SPARSE_VOXEL_PROBE_OCTAHEDRAL_SIZE));
}

// Cell centre direction in the probe's frame. Deterministic and jitter-free:
// this is the direction a *stored* radiance cell is understood to represent,
// which is what makes a neighbouring probe able to reconstruct a hit point from
// it a frame later.
fn sparse_voxel_probe_cell_direction(
    normal : vec3<f32>, cellIndex : u32) -> vec3<f32> {
    let uv = (sparse_voxel_probe_cell_coord(cellIndex) + vec2<f32>(0.5)) /
        f32(SPARSE_VOXEL_PROBE_OCTAHEDRAL_SIZE);
    return sparse_voxel_probe_basis(normal) *
        sparse_voxel_probe_hemioct_decode(uv);
}

// Which cell a world direction belongs to, or 0xffffffff when it points below
// the probe's horizon.
fn sparse_voxel_probe_cell_from_direction(
    normal : vec3<f32>, direction : vec3<f32>) -> u32 {
    let basis = sparse_voxel_probe_basis(normal);
    let local = vec3<f32>(
        dot(direction, basis[0]), dot(direction, basis[1]),
        dot(direction, basis[2]));
    if (local.z <= 0.0) { return 0xffffffffu; }
    let uv = clamp(
        sparse_voxel_probe_hemioct_encode(local), vec2<f32>(0.0),
        vec2<f32>(0.9999));
    let cell = vec2<u32>(uv * f32(SPARSE_VOXEL_PROBE_OCTAHEDRAL_SIZE));
    return min(cell.y, SPARSE_VOXEL_PROBE_OCTAHEDRAL_SIZE - 1u) *
        SPARSE_VOXEL_PROBE_OCTAHEDRAL_SIZE +
        min(cell.x, SPARSE_VOXEL_PROBE_OCTAHEDRAL_SIZE - 1u);
}

fn sparse_voxel_probe_hash(value : u32) -> u32 {
    var bits = value;
    bits = (bits ^ (bits >> 16u)) * 0x7feb352du;
    bits = (bits ^ (bits >> 15u)) * 0x846ca68bu;
    return bits ^ (bits >> 16u);
}

// The four-frame jitter cycle, following Lumen.
//
// A per-frame random offset and a four-frame cycle both average to the same
// thing, and they behave completely differently. Random offsets are only
// stratified in expectation: consecutive frames can land almost on top of each
// other or at opposite corners, so what a probe measures changes by an
// unpredictable amount every frame and never repeats. A fixed cycle visits the
// four quadrants exactly once and then returns, which makes the sequence
// periodic - and a periodic sequence is one a temporal filter can actually
// converge against.
const SPARSE_VOXEL_PROBE_JITTER_PHASES : u32 = 4u;

// Quadrant centres in a rotated order, so consecutive frames land in diagonally
// opposite quadrants rather than adjacent ones. That maximises the distance
// between successive samples, which is what makes a two-frame average already
// cover the cell instead of clumping in one half of it.
fn sparse_voxel_probe_cycle_offset(phase : u32) -> vec2<f32> {
    switch (phase & 3u) {
        case 0u: { return vec2<f32>(0.25, 0.25); }
        case 1u: { return vec2<f32>(0.75, 0.75); }
        case 2u: { return vec2<f32>(0.75, 0.25); }
        default: { return vec2<f32>(0.25, 0.75); }
    }
}

// Each tile advances the cycle from its own starting phase - the screen-space
// equivalent of Lumen's per-page frame index.
//
// Without it every tile jitters in lockstep, so whatever the jitter does to a
// probe it does to all of them on the same frame. A residual error that would
// have been invisible as spatial noise instead becomes the whole image
// pulsing together, which is exactly the artefact temporal filters are worst
// at removing. Offsetting the phase per tile turns that pulse back into noise.
fn sparse_voxel_probe_tile_phase(tileCoord : vec2<u32>, frame : u32) -> u32 {
    let offset = sparse_voxel_probe_hash(
        tileCoord.x * 0x9e3779b9u ^ (tileCoord.y * 0x85ebca6bu));
    return (frame + offset) & (SPARSE_VOXEL_PROBE_JITTER_PHASES - 1u);
}

// A stratified direction for one probe cell. Each cell owns one hemi-octahedral
// texel, and the jitter moves the sample inside that texel so the probe
// integrates the whole hemisphere over time without ever leaving its own
// stratum. That is what keeps a probe's estimate stable: the direction set is
// fixed, only the sample point inside each cell moves.
//
// The sample point is the cycle's quadrant centre plus a dither that fills that
// quadrant, so four consecutive frames cover the cell uniformly by
// construction rather than only on average.
fn sparse_voxel_probe_direction(
    normal : vec3<f32>, cellIndex : u32, phase : u32, seed : u32,
    frame : u32) -> vec3<f32> {
    let bits = sparse_voxel_probe_hash(
        (cellIndex * 0x9e3779b9u) ^ (seed * 0x85ebca6bu) ^
        (frame * 0xc2b2ae35u));
    let dither = (vec2<f32>(
        f32(bits & 0xffffu) / 65536.0,
        f32((bits >> 16u) & 0xffffu) / 65536.0) - vec2<f32>(0.5)) * 0.5;
    let uv = (sparse_voxel_probe_cell_coord(cellIndex) +
        sparse_voxel_probe_cycle_offset(phase) + dither) /
        f32(SPARSE_VOXEL_PROBE_OCTAHEDRAL_SIZE);
    return sparse_voxel_probe_basis(normal) *
        sparse_voxel_probe_hemioct_decode(uv);
}

// Which cells this frame spend their ray on next-event estimation instead of
// on the stratified hemisphere sweep. The reserved set is a strided subset that
// rotates every frame rather than a contiguous tail: a fixed tail permanently
// blanks one region of the hemisphere, and no amount of temporal accumulation
// recovers the sky that region was supposed to measure.
fn sparse_voxel_probe_is_next_event_cell(
    cellIndex : u32, slots : u32, frame : u32) -> bool {
    if (slots == 0u) { return false; }
    if (slots >= SPARSE_VOXEL_PROBE_DIRECTION_COUNT) { return true; }
    let stride = SPARSE_VOXEL_PROBE_DIRECTION_COUNT / slots;
    return (cellIndex + frame) % stride == 0u;
}

fn sparse_voxel_probe_luminance(value : vec3<f32>) -> f32 {
    return dot(value, vec3<f32>(0.2126, 0.7152, 0.0722));
}

// Per-probe scheduling state. Mirrors SparseVoxelProbeScheduleGPU.
struct SparseVoxelProbeSchedule {
    lightSignature : u32,
    meanLuminance : f32,
    secondMomentLuminance : f32,
    counters : u32,
}

const SPARSE_VOXEL_PROBE_COUNTER_MASK : u32 = 0xffu;
const SPARSE_VOXEL_PROBE_WAKE_SHIFT : u32 = 8u;
const SPARSE_VOXEL_PROBE_TIER_SHIFT : u32 = 16u;
// One-frame marker set by ray scheduling when the probe's lighting signature
// changes. Resolve consumes it to discard directional-map history that belongs
// to a different set of lights; convergence repacking clears it afterwards.
const SPARSE_VOXEL_PROBE_LIGHT_CHANGED : u32 = 1u << 24u;

// Update tiers, in increasing order of how settled a probe is. The tier is
// what both the interval and the per-frame ray count are looked up from, and
// what the sharing pass reads to decide a probe no longer needs to borrow its
// neighbours' rays.
const SPARSE_VOXEL_PROBE_TIER_AWAKE : u32 = 0u;
const SPARSE_VOXEL_PROBE_TIER_CONVERGED : u32 = 1u;
const SPARSE_VOXEL_PROBE_TIER_ASLEEP : u32 = 2u;

fn sparse_voxel_probe_stable_runs(counters : u32) -> u32 {
    return counters & SPARSE_VOXEL_PROBE_COUNTER_MASK;
}

fn sparse_voxel_probe_wake_frames(counters : u32) -> u32 {
    return (counters >> SPARSE_VOXEL_PROBE_WAKE_SHIFT) &
        SPARSE_VOXEL_PROBE_COUNTER_MASK;
}

fn sparse_voxel_probe_last_tier(counters : u32) -> u32 {
    return (counters >> SPARSE_VOXEL_PROBE_TIER_SHIFT) &
        SPARSE_VOXEL_PROBE_COUNTER_MASK;
}

fn sparse_voxel_probe_light_changed(counters : u32) -> bool {
    return (counters & SPARSE_VOXEL_PROBE_LIGHT_CHANGED) != 0u;
}

fn sparse_voxel_probe_pack_counters(
    stableRuns : u32, wakeFrames : u32, tier : u32) -> u32 {
    return min(stableRuns, SPARSE_VOXEL_PROBE_COUNTER_MASK) |
        (min(wakeFrames, SPARSE_VOXEL_PROBE_COUNTER_MASK) <<
            SPARSE_VOXEL_PROBE_WAKE_SHIFT) |
        (min(tier, SPARSE_VOXEL_PROBE_COUNTER_MASK) <<
            SPARSE_VOXEL_PROBE_TIER_SHIFT);
}

fn sparse_voxel_probe_empty_schedule() -> SparseVoxelProbeSchedule {
    return SparseVoxelProbeSchedule(0u, 0.0, 0.0, 0u);
}

struct SparseVoxelProbeCellSchedule {
    traced : bool,
    nextEvent : bool,
    // Next-event cells the probe traces this frame. The emitter estimator
    // divides by the number of slots that sample it, and once the ray count is
    // reduced that is this budget rather than the configured slot count -
    // dividing by the latter scales every finite light down by exactly the
    // fraction of slots that were dropped.
    nextEventCount : u32,
}

// Whether this cell is one of the `traceCount` directions the probe traces this
// frame, and which estimator it serves if so.
//
// The budget is split *explicitly* between the two estimators rather than by
// intersecting two strides, and that is the whole difficulty of reducing a
// probe's ray count.
//
// The next-event set is the cells where `(cell + frame) % (64/slots) == 0`, a
// congruence class mod a power of two. Any equally natural way of picking N of
// 64 cells - a stride, a rotated window, a bit-reversal - also lands on a
// congruence class mod a power of two, and two such classes are either
// identical or disjoint. So the obvious selections give a reduced probe either
// *only* next-event cells, which measures no sky at all, or *no* next-event
// cells, which measures no finite emitter at all. Both look like a plausible
// image at first and are simply missing half the lighting.
//
// Ranking each cell within its own class and taking a rotating slice of each
// keeps the mix at any ray count, and reduces exactly to the full set when
// `traceCount` is 64.
fn sparse_voxel_probe_cell_schedule(
    cellIndex : u32, traceCount : u32, neeSlots : u32,
    frame : u32) -> SparseVoxelProbeCellSchedule {
    let total = SPARSE_VOXEL_PROBE_DIRECTION_COUNT;
    let count = min(traceCount, total);
    let slots = min(neeSlots, total / 2u);
    let isNextEvent = sparse_voxel_probe_is_next_event_cell(
        cellIndex, slots, frame);
    if (count == 0u) {
        return SparseVoxelProbeCellSchedule(false, isNextEvent, 0u);
    }
    if (count >= total) {
        return SparseVoxelProbeCellSchedule(true, isNextEvent, slots);
    }

    // Same proportion the full direction set uses, but never less than one
    // next-event slot while any emitter is being sampled: the ratio being a
    // little off costs variance, whereas rounding it to zero costs a probe all
    // of its finite-light contribution for that frame.
    let stratifiedSize = total - slots;
    var nextEventBudget = 0u;
    if (slots != 0u) { nextEventBudget = max((count * slots) / total, 1u); }
    nextEventBudget = min(nextEventBudget, slots);
    let stratifiedBudget = min(count - nextEventBudget, stratifiedSize);

    // The next-event class this frame is `first`, `first + stride`, ... The
    // stride and the phase have to match sparse_voxel_probe_is_next_event_cell
    // exactly, or a cell is ranked inside a class it does not belong to.
    let stride = select(total, total / max(slots, 1u), slots != 0u);
    let first = select(0u, (stride - (frame % stride)) % stride, slots != 0u);

    var ordinal = 0u;
    var classSize = 0u;
    var budget = 0u;
    if (isNextEvent) {
        ordinal = (cellIndex - first) / stride;
        classSize = slots;
        budget = nextEventBudget;
    } else {
        var nextEventBelow = 0u;
        if (slots != 0u && cellIndex > first) {
            nextEventBelow = (cellIndex - 1u - first) / stride + 1u;
        }
        ordinal = cellIndex - nextEventBelow;
        classSize = stratifiedSize;
        budget = stratifiedBudget;
    }
    if (classSize == 0u || budget == 0u) {
        return SparseVoxelProbeCellSchedule(false, isNextEvent, nextEventBudget);
    }
    // Advance by the budget each frame so consecutive frames take disjoint
    // slices and the class is swept completely every classSize/budget frames.
    // A probe at a reduced ray count therefore still covers its whole
    // hemisphere, just over several frames instead of one.
    let rotation = (frame * budget) % classSize;
    return SparseVoxelProbeCellSchedule(
        ((ordinal + rotation) % classSize) < budget, isNextEvent,
        nextEventBudget);
}

// How settled this probe is. The tier is what both the update interval and the
// per-frame ray count are looked up from.
//
// The tiers are driven by how many *consecutive traced updates* agreed, not by
// how long the probe has existed. That distinction matters: a probe under a
// flickering emitter accumulates sample count just as fast as one under a
// static sky, and only the run length tells them apart.
//
// `thresholds` is x minimum traced updates, y maximum relative standard error,
// z runs to reach converged, w runs to reach asleep.
fn sparse_voxel_probe_tier(
    schedule : SparseVoxelProbeSchedule, sampleCount : u32,
    hasHistory : bool, signatureChanged : bool,
    thresholds : vec4<f32>) -> u32 {
    // A probe with no predecessor has no estimate at all, and one whose lights
    // moved has an estimate of a scene that no longer exists. Both have to
    // measure before anything else here is meaningful.
    if (!hasHistory || signatureChanged) {
        return SPARSE_VOXEL_PROBE_TIER_AWAKE;
    }
    if (sparse_voxel_probe_wake_frames(schedule.counters) != 0u) {
        return SPARSE_VOXEL_PROBE_TIER_AWAKE;
    }
    if (f32(sampleCount) < max(thresholds.x, 1.0)) {
        return SPARSE_VOXEL_PROBE_TIER_AWAKE;
    }
    let runs = sparse_voxel_probe_stable_runs(schedule.counters);
    if (f32(runs) >= max(thresholds.w, 1.0)) {
        return SPARSE_VOXEL_PROBE_TIER_ASLEEP;
    }
    if (f32(runs) >= max(thresholds.z, 1.0)) {
        return SPARSE_VOXEL_PROBE_TIER_CONVERGED;
    }
    return SPARSE_VOXEL_PROBE_TIER_AWAKE;
}

// Frames between successive measurements of any one direction cell.
//
// A probe tracing `rays` of its 64 directions per update covers a given cell
// once every 64/rays updates, and updates come every `interval` frames. This is
// the unit the per-cell temporal blend has to be expressed in - blending as
// though every cell were measured every frame is what makes a sparsely traced
// probe hold a wrong value for hundreds of frames.
fn sparse_voxel_probe_cell_period(
    rayCounts : vec4<u32>, intervals : vec4<u32>, tier : u32) -> u32 {
    let rays = max(
        min(sparse_voxel_probe_tier_value(rayCounts, tier),
            SPARSE_VOXEL_PROBE_DIRECTION_COUNT), 1u);
    let interval = max(sparse_voxel_probe_tier_value(intervals, tier), 1u);
    return (SPARSE_VOXEL_PROBE_DIRECTION_COUNT / rays) * interval;
}

// Per-tier lookup out of a settings vec4 indexed by tier.
fn sparse_voxel_probe_tier_value(values : vec4<u32>, tier : u32) -> u32 {
    switch (tier) {
        case 1u: { return values.y; }
        case 2u: { return values.z; }
        default: { return values.x; }
    }
}

// Whether this probe's turn comes up on this frame.
//
// The phase is the probe's tile rather than its index because a refined probe's
// index is reallocated every frame; two consecutive frames would disagree about
// whose turn it was and the interval would decay into a coin flip. The tile is
// also what spreads the work: neighbouring tiles hash to different phases, so
// each frame traces roughly 1/interval of the probes scattered over the screen
// instead of a contiguous band of them.
fn sparse_voxel_probe_traces_this_frame(
    tileCoord : vec2<u32>, interval : u32, frame : u32) -> bool {
    let period = max(interval, 1u);
    if (period == 1u) { return true; }
    let phase = sparse_voxel_probe_hash(
        tileCoord.x * 0x9e3779b9u ^ (tileCoord.y * 0x85ebca6bu));
    return ((frame + phase) % period) == 0u;
}

// Fold one traced update into the probe's convergence statistics.
//
// The weight is 1/(n+1) over the probe's run on this surface, matching the
// per-cell radiance blend, so the error estimate settles on the same timescale
// as the signal it is measuring. Comparing against the probe's own mean rather
// than an absolute threshold is what keeps the test meaningful in both a bright
// exterior and a dim interior.
fn sparse_voxel_probe_accumulate_convergence(
    schedule : SparseVoxelProbeSchedule, luminance : f32, sampleCount : u32,
    thresholds : vec4<f32>, blendFloor : f32) -> SparseVoxelProbeSchedule {
    let alpha = max(
        1.0 / (f32(min(sampleCount, SPARSE_VOXEL_PROBE_MAXIMUM_SAMPLES)) + 1.0),
        clamp(blendFloor, 0.01, 1.0));
    let mean = mix(schedule.meanLuminance, luminance, alpha);
    let secondMoment = mix(
        schedule.secondMomentLuminance, luminance * luminance, alpha);
    let variance = max(secondMoment - mean * mean, 0.0);
    // The floor keeps a probe in near-darkness from reporting enormous relative
    // error over radiance differences far below anything the output can show.
    let relativeError = sqrt(variance) / max(mean, 1.0e-3);
    var runs = sparse_voxel_probe_stable_runs(schedule.counters);
    if (relativeError <= max(thresholds.y, 0.01)) {
        runs = min(runs + 1u, SPARSE_VOXEL_PROBE_COUNTER_MASK);
    } else {
        runs = 0u;
    }
    // Waking is spent by elapsed frames, not by traced updates, so a woken
    // probe that also happens to be off-phase still returns to its tier.
    let wake = sparse_voxel_probe_wake_frames(schedule.counters);
    return SparseVoxelProbeSchedule(
        schedule.lightSignature, mean, secondMoment,
        sparse_voxel_probe_pack_counters(
            runs, wake, sparse_voxel_probe_last_tier(schedule.counters)));
}

// Traced cells this frame below which the firefly reference is not trusted.
//
// The clamp compares a cell against the mean of the cells traced alongside it,
// so that mean has to describe the hemisphere. Once the ray count is reduced a
// probe may trace only a few directions per frame, and a mean over three of
// them says nothing - if they all happened to be occluded the reference is
// near zero and every lit cell is clamped to black.
const SPARSE_VOXEL_PROBE_FIREFLY_MINIMUM_SAMPLES : u32 = 8u;

// Firefly suppression, expressed as a ratio against the probe's own mean rather
// than as an absolute box. A single sample that came back many times brighter
// than everything else the probe measured is, at one sample, indistinguishable
// from a real feature - but it carries variance out of all proportion to the
// energy it represents, and a diffuse probe is the wrong place to preserve it.
//
// The threshold has to scale with the probe: an absolute box tuned for a lit
// exterior erases the entire signal in a dim interior, and one tuned for the
// interior does nothing outdoors. Scaling by the mean and clamping only the
// magnitude - never the hue - keeps the suppression proportional at any
// exposure.
fn sparse_voxel_probe_clamp_firefly(
    value : vec3<f32>, meanLuminance : f32, ratio : f32) -> vec3<f32> {
    let luminance = sparse_voxel_probe_luminance(value);
    let limit = meanLuminance * max(ratio, 1.0);
    if (luminance <= limit || luminance <= 1.0e-6) { return value; }
    return value * (limit / luminance);
}
