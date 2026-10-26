#include "sparse_voxel_types.wgsl"
#include "sparse_voxel_probe_common.wgsl"

// Probe spawn, following Brixelizer GI's FfxBrixelizerGISpawnScreenProbes for
// the uniform grid and Lumen's screen probe placement for the refinement.
//
// The seed pixel is chosen *stochastically*, not by electing the pixel nearest
// the tile centre: a low-discrepancy jitter picks a candidate inside the tile
// and the first covered one wins, with the jitter advancing every frame.
//
// That detail is the whole point rather than an implementation quirk. A fixed
// seed pixel makes every frame re-measure the same receiver, which is exactly
// the failure the per-pixel path has - it is why shaking the camera makes the
// current renderer converge faster, because the shake is accidentally doing
// what this jitter does deliberately. Moving the seed pixel each frame means
// consecutive probe updates sample different points of the same surface patch,
// so the temporal accumulation averages over the tile's area instead of
// re-averaging one point forever.
//
// Refinement then asks the question the uniform grid cannot: does this tile's
// probe actually represent this tile? Every covered pixel is tested against
// the probe with the same acceptance test interpolation will apply, and the
// pixels that fail get probes of their own, drawn from a shared overflow
// budget. A tile lying flat costs nothing; a tile straddling a pillar edge, a
// railing or a stair nosing gets the two or three surfaces it actually
// contains described separately.
//
// This is what removes tile structure from scenes with strong depth variance.
// No amount of filtering can recover a surface that no probe ever measured,
// and before refinement those pixels were served by whichever probe happened
// to be nearest - a probe describing a different surface entirely.

struct SparseVoxelProbePlaceSettings {
    // xy raster dimensions, z probe tile size, w frame index.
    dimensionsTile : vec4<u32>,
    // x probes across, y probes down, z history valid, w refined probe budget.
    probeCounts : vec4<u32>,
    // x normal threshold, y relative depth threshold, z probe world radius
    // scale, w plane distance scale used by the representability test.
    thresholds : vec4<f32>,
    // x: write the placement diagnostics below.
    // y: offer the previous frame's seed pixel as an election candidate.
    // z: maximum distance-density grid stride (1, 2 or 4).
    flags : vec4<u32>,
    // x distance where density becomes one probe per 2x2 tiles, y where it
    // becomes one per 4x4. zw are lighting distances used by later passes.
    distances : vec4<f32>,
    // x cosine of the angle at which a normal becomes a distinct face,
    // y enables deterministic face-cluster refinement.
    faceSettings : vec4<f32>,
}

@group(0) @binding(0) var<uniform> raster : SparseVoxelRasterUniforms;
@group(0) @binding(1) var<uniform> settings : SparseVoxelProbePlaceSettings;
@group(0) @binding(2) var<storage, read> coverage : array<u32>;
@group(0) @binding(3) var<storage, read> linearDepth : array<f32>;
@group(0) @binding(4) var geometryNormalTexture : texture_2d<f32>;
@group(0) @binding(5) var instanceIdTexture : texture_2d<u32>;
@group(0) @binding(6) var voxelIdTexture : texture_2d<u32>;
@group(0) @binding(7) var motionVectorTexture : texture_2d<f32>;
@group(0) @binding(8) var<storage, read> previousProbes : array<SparseVoxelProbe>;
@group(0) @binding(9) var<storage, read_write> probes : array<SparseVoxelProbe>;
@group(0) @binding(10) var<storage, read_write> allocation : array<atomic<u32>>;
@group(0) @binding(11) var<storage, read_write> adaptiveTiles : array<u32>;
@group(0) @binding(12) var<storage, read> previousAdaptiveTiles : array<u32>;
// Placement-side diagnostics, per screen region on the same 4x4 grid the gather
// uses. Everything measured so far has been from the consumer side, which can
// only ever say "no probe served this pixel"; these say whether the descent
// even asked for one. Counted per tile, not per pixel, so the cost is trivial.
@group(0) @binding(13) var<storage, read_write> diagnostics : array<atomic<u32>>;

const PROBE_PLACE_DIAGNOSTIC_REGION_AXIS : u32 = 4u;
const PROBE_PLACE_DIAGNOSTIC_WORDS_PER_REGION : u32 = 5u;
const PROBE_PLACE_DIAGNOSTIC_TILES : u32 = 0u;
const PROBE_PLACE_DIAGNOSTIC_COVERED : u32 = 1u;
// Pixels the first refinement level found unrepresented by the tile probe.
const PROBE_PLACE_DIAGNOSTIC_UNREPRESENTED : u32 = 2u;
const PROBE_PLACE_DIAGNOSTIC_PLACED : u32 = 3u;
// Wanted a refined probe and could not have one.
const PROBE_PLACE_DIAGNOSTIC_STARVED : u32 = 4u;
const PROBE_PLACE_DIAGNOSTIC_WORDS : u32 = 80u;

fn probe_place_diagnostic_region(
    tileOrigin : vec2<u32>, dimensions : vec2<u32>) -> u32 {
    let cell = min(
        tileOrigin * PROBE_PLACE_DIAGNOSTIC_REGION_AXIS /
            max(dimensions, vec2<u32>(1u)),
        vec2<u32>(PROBE_PLACE_DIAGNOSTIC_REGION_AXIS - 1u));
    return (cell.y * PROBE_PLACE_DIAGNOSTIC_REGION_AXIS + cell.x) *
        PROBE_PLACE_DIAGNOSTIC_WORDS_PER_REGION;
}

@compute @workgroup_size(64, 1, 1)
fn resetSparseVoxelProbePlaceDiagnostics(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    if (globalId.x >= PROBE_PLACE_DIAGNOSTIC_WORDS) { return; }
    atomicStore(&diagnostics[globalId.x], 0u);
}

const PROBE_PLACE_GROUP : u32 = 64u;
// Brixelizer tries eight jittered candidates before giving the tile up.
const PROBE_PLACE_ATTEMPTS : u32 = 8u;
// Let a new probe integrate a representative set of receivers before its seed
// becomes stationary. This is observations on the same surface, not wall-clock
// frames, because identityState.w is reset by a real disocclusion.
const PROBE_PLACE_STABLE_SAMPLES : u32 = 16u;

// Election scores carry the candidate's *tile-local* pixel, never its screen
// pixel.
//
// They used to carry the screen pixel in ten bits per axis. An x coordinate
// needs eleven bits at 1280 wide, so from x = 1024 rightwards every candidate
// spilled x's high bit into the y field, and the winner decoded to a pixel 1024
// columns to the left and one row down. Those tiles' probes were built from a
// surface on the far side of the screen: valid, plausibly oriented, and
// nowhere near the pixels they were supposed to light - which is the
// rightmost-19%-of-the-screen band of flat, too-dark shading, and why it was
// pinned to a screen fraction that no camera, tile size or budget moved and
// vanished entirely at 640x360.
//
// A tile-local coordinate is bounded by the tile size, clamped to 32 where it
// is authored, so eight bits per axis cannot overflow at any render
// resolution. Fields are ordered most significant first so one atomicMin ranks
// candidates by surface continuity, then attempt, then position.
const PROBE_PLACE_LOCAL_BITS : u32 = 8u;
const PROBE_PLACE_LOCAL_MASK : u32 = 0xffu;
// Four bits, so the previous-frame seed offered below at PROBE_PLACE_ATTEMPTS
// stays inside its own field instead of setting the continuity bit.
const PROBE_PLACE_ATTEMPT_SHIFT : u32 = 2u * PROBE_PLACE_LOCAL_BITS;
const PROBE_PLACE_CONTINUES_SHIFT : u32 = PROBE_PLACE_ATTEMPT_SHIFT + 4u;
// Refinement ranks by distance from the region centroid instead of by attempt,
// and has the whole upper half of the word for it.
const PROBE_PLACE_RANK_SHIFT : u32 = 2u * PROBE_PLACE_LOCAL_BITS;

fn probe_place_pack_local(local : vec2<u32>) -> u32 {
    return ((local.y & PROBE_PLACE_LOCAL_MASK) << PROBE_PLACE_LOCAL_BITS) |
        (local.x & PROBE_PLACE_LOCAL_MASK);
}

fn probe_place_unpack_pixel(
    score : u32, tileOrigin : vec2<u32>, dimensions : vec2<u32>) -> vec2<u32> {
    let local = vec2<u32>(
        score & PROBE_PLACE_LOCAL_MASK,
        (score >> PROBE_PLACE_LOCAL_BITS) & PROBE_PLACE_LOCAL_MASK);
    return min(tileOrigin + local, dimensions - vec2<u32>(1u));
}

fn probe_normalize(value : vec3<f32>) -> vec3<f32> {
    let length2 = dot(value, value);
    return select(
        vec3<f32>(0.0, 1.0, 0.0),
        value * inverseSqrt(max(length2, 1.0e-12)), length2 > 1.0e-12);
}

fn probe_face_class(normal : vec3<f32>) -> u32 {
    let magnitude = abs(normal);
    var axis = 0u;
    if (magnitude.y > magnitude.x) { axis = 1u; }
    if (magnitude.z > max(magnitude.x, magnitude.y)) { axis = 2u; }
    return axis | select(0u, 4u, normal[axis] < 0.0);
}

fn probe_face_normal(faceClass : u32) -> vec3<f32> {
    let axis = faceClass & 3u;
    let sign = select(1.0, -1.0, (faceClass & 4u) != 0u);
    if (axis == 0u) { return vec3<f32>(sign, 0.0, 0.0); }
    if (axis == 1u) { return vec3<f32>(0.0, sign, 0.0); }
    return vec3<f32>(0.0, 0.0, sign);
}

fn probe_face_is_split(primaryFace : u32, candidateFace : u32) -> bool {
    return candidateFace != primaryFace &&
        dot(probe_face_normal(primaryFace), probe_face_normal(candidateFace)) <=
            settings.faceSettings.x + 1.0e-4;
}

fn probe_pcg(value : u32) -> u32 {
    let state = value * 747796405u + 2891336453u;
    let word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Radical-inverse base two, the second Hammersley coordinate.
fn probe_radical_inverse(value : u32) -> f32 {
    var bits = value;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xaaaaaaaau) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xccccccccu) >> 2u);
    bits = ((bits & 0x0f0f0f0fu) << 4u) | ((bits & 0xf0f0f0f0u) >> 4u);
    bits = ((bits & 0x00ff00ffu) << 8u) | ((bits & 0xff00ff00u) >> 8u);
    return f32(bits) * 2.3283064365386963e-10;
}

fn probe_hammersley(index : u32) -> vec2<f32> {
    let i = index & 63u;
    return vec2<f32>(f32(i) / 64.0, probe_radical_inverse(i));
}

// Election scratch. The score packs "distance from tile centre" into the high
// bits so a single atomicMin picks the winner without a second reduction.
var<workgroup> bestScore : atomic<u32>;
var<workgroup> coveredCount : atomic<u32>;
// Full-tile complexity, separate from the eight placement candidates above.
// A straight silhouette contributes O(tileSize) transitions; sub-pixel voxel
// detail contributes O(tileSize^2). That distinction lets distance LOD keep a
// large clean edge sparse while retaining probes where tiny faces alternate.
var<workgroup> tileSurfacePixels : atomic<u32>;
var<workgroup> tileNormalTransitions : atomic<u32>;
// Full-tile axis/sign clusters. This sees tiny faces even when the stochastic
// primary election's eight candidates do not land on them.
var<workgroup> faceCount : array<atomic<u32>, 6>;
var<workgroup> faceCentroidX : array<atomic<u32>, 6>;
var<workgroup> faceCentroidY : array<atomic<u32>, 6>;
var<workgroup> faceScore : array<atomic<u32>, 6>;
var<workgroup> selectedFaceClasses : array<u32, SPARSE_VOXEL_PROBE_ADAPTIVE_PER_TILE>;
var<workgroup> selectedFaceCount : u32;
// Refinement scratch, one entry per region at the level being subdivided: how
// many of the region's covered pixels the probes above it fail to represent,
// where those pixels sit, and which one best represents them.
var<workgroup> regionCount : array<atomic<u32>, SPARSE_VOXEL_PROBE_ADAPTIVE_REGIONS>;
var<workgroup> regionCentroidX : array<atomic<u32>, SPARSE_VOXEL_PROBE_ADAPTIVE_REGIONS>;
var<workgroup> regionCentroidY : array<atomic<u32>, SPARSE_VOXEL_PROBE_ADAPTIVE_REGIONS>;
var<workgroup> regionError : array<atomic<u32>, SPARSE_VOXEL_PROBE_ADAPTIVE_REGIONS>;
var<workgroup> regionScore : array<atomic<u32>, SPARSE_VOXEL_PROBE_ADAPTIVE_REGIONS>;
// Probes this tile has committed, so deeper levels can tell what is already
// described and stop descending.
var<workgroup> refinePosition : array<vec4<f32>, SPARSE_VOXEL_PROBE_ADAPTIVE_PER_TILE>;
var<workgroup> refineNormal : array<vec4<f32>, SPARSE_VOXEL_PROBE_ADAPTIVE_PER_TILE>;
var<workgroup> refinePlaced : u32;
// Set when a candidate was refused for budget. Descending past that point would
// ask for four probes to cover what one was just refused for.
var<workgroup> refineStarved : u32;

fn probe_world_position(pixel : vec2<u32>, depth : f32) -> vec3<f32> {
    let cameraDirection = vec3<f32>(
        (f32(pixel.x) + 0.5 - raster.focalPrincipal.z) / raster.focalPrincipal.x,
        (f32(pixel.y) + 0.5 - raster.focalPrincipal.w) / raster.focalPrincipal.y,
        1.0);
    let worldDirection = normalize(
        (raster.cameraToWorld * vec4<f32>(cameraDirection, 0.0)).xyz);
    return raster.cameraWorldPositionNear.xyz + worldDirection * depth;
}

fn probe_view_direction(pixel : vec2<u32>) -> vec3<f32> {
    let cameraDirection = vec3<f32>(
        (f32(pixel.x) + 0.5 - raster.focalPrincipal.z) / raster.focalPrincipal.x,
        (f32(pixel.y) + 0.5 - raster.focalPrincipal.w) / raster.focalPrincipal.y,
        1.0);
    return normalize((raster.cameraToWorld * vec4<f32>(cameraDirection, 0.0)).xyz);
}

fn probe_identity(pixel : vec2<i32>) -> vec2<u32> {
    let normal = textureLoad(geometryNormalTexture, pixel, 0).xyz;
    let faceClass = probe_face_class(normal);
    let instance = textureLoad(instanceIdTexture, pixel, 0).x;
    return vec2<u32>(
        (instance & ((1u << 29u) - 1u)) | (faceClass << 29u),
        textureLoad(voxelIdTexture, pixel, 0).x);
}

// World radius a probe placed at this depth is allowed to serve, for a
// footprint of the given width in pixels. A footprint subtends a fixed angle,
// so its world size grows linearly with depth.
//
// The footprint is the probe's real one, not always the tile's. A refined probe
// covers the handful of pixels its quadrant could not represent, and giving it
// a whole tile's radius lets it out-weight the probes that genuinely own the
// surrounding surface - the interpolation would accept it for pixels it knows
// nothing about. Sizing the radius to the region actually served is what keeps
// a refined probe local to the detail it was placed for.
fn probe_radius(depth : f32, footprintPixels : u32) -> f32 {
    return max(
        depth * f32(max(footprintPixels, 1u)) /
            max(raster.focalPrincipal.x, 1.0e-3) *
            max(settings.thresholds.z, 0.5),
        1.0e-3);
}

// The previous-frame probe this surface point should inherit history from, or
// 0xffffffff. The seed moves inside its tile as the placement pattern jitters,
// and a sub-pixel reprojection can put that seed on either side of a tile
// boundary. Searching only the single reprojected tile can therefore reset a
// valid predecessor during motion. Search the immediate neighbourhood and let
// the strict surface predicate below prevent history from crossing geometry.
fn probe_history_index(
    pixel : vec2<u32>, depth : f32, position : vec3<f32>, normal : vec3<f32>,
    identity : vec2<u32>, tolerance : f32) -> u32 {
    if (settings.probeCounts.z == 0u) { return 0xffffffffu; }
    let dimensions = max(settings.dimensionsTile.xy, vec2<u32>(1u));
    let tileSize = max(settings.dimensionsTile.z, 1u);
    let probeCounts = settings.probeCounts.xy;
    let motion = textureLoad(motionVectorTexture, vec2<i32>(pixel), 0).xy;
    let uv = (vec2<f32>(pixel) + vec2<f32>(0.5)) / vec2<f32>(dimensions);
    let historyUv = uv - motion;
    if (any(historyUv < vec2<f32>(0.0)) || any(historyUv >= vec2<f32>(1.0))) {
        return 0xffffffffu;
    }
    let historyPixel = vec2<u32>(historyUv * vec2<f32>(dimensions));
    let historyProbeCoord = vec2<i32>(min(
        historyPixel / tileSize, probeCounts - vec2<u32>(1u)));
    let maximumCoord = vec2<i32>(probeCounts) - vec2<i32>(1);
    var best = 0xffffffffu;
    var bestDistance = 1.0e30;
    for (var offsetY = -1; offsetY <= 1; offsetY += 1) {
        for (var offsetX = -1; offsetX <= 1; offsetX += 1) {
            let coord = vec2<u32>(clamp(
                historyProbeCoord + vec2<i32>(offsetX, offsetY),
                vec2<i32>(0), maximumCoord));
            let candidate = sparse_voxel_probe_index(coord, probeCounts);
            if (candidate < arrayLength(&previousProbes)) {
                let previous = previousProbes[candidate];
                if (probe_matches_surface(
                        previous, identity, position, normal, depth,
                        tolerance)) {
                    let separation = distance(
                        position, previous.positionDepth.xyz);
                    if (separation < bestDistance ||
                        (separation == bestDistance && candidate < best)) {
                        bestDistance = separation;
                        best = candidate;
                    }
                }
            }
            // A face may move from a secondary lane to the primary lane when
            // jitter changes the winning seed. Search the previous secondary
            // registry too so that role change keeps the same radiance cache.
            let base = candidate * SPARSE_VOXEL_PROBE_ADAPTIVE_TILE_STRIDE;
            if (base >= arrayLength(&previousAdaptiveTiles)) { continue; }
            let count = min(
                previousAdaptiveTiles[base],
                SPARSE_VOXEL_PROBE_ADAPTIVE_PER_TILE);
            for (var i = 0u; i < count; i += 1u) {
                let entry = base + 1u + i;
                if (entry >= arrayLength(&previousAdaptiveTiles)) { break; }
                let refinedCandidate = previousAdaptiveTiles[entry];
                if (refinedCandidate >= arrayLength(&previousProbes)) {
                    continue;
                }
                let previous = previousProbes[refinedCandidate];
                if (!probe_matches_surface(
                        previous, identity, position, normal, depth,
                        tolerance)) { continue; }
                let separation = distance(
                    position, previous.positionDepth.xyz);
                if (separation < bestDistance ||
                    (separation == bestDistance && refinedCandidate < best)) {
                    bestDistance = separation;
                    best = refinedCandidate;
                }
            }
        }
    }
    return best;
}

// Does this surface point belong to the same surface a probe already describes?
//
// Deliberately a *surface* test, not a voxel test. Matching the voxel id looks
// stricter and therefore safer, and it is neither: the probe's seed pixel moves
// within its tile every frame by design, so it lands on a different voxel
// almost every frame. Where voxels are small - which is exactly where the
// geometry is detailed and the lighting hardest - the voxel id essentially
// never repeats, so every probe is declared a new surface, history is thrown
// away, and each frame's estimate is a raw 64-ray sample with nothing behind
// it. That is what makes detailed regions flicker between light and dark while
// flat ones sit still.
//
// Instance and face class still have to match, and the point still has to lie
// on the probe's plane and at a compatible depth. A probe cannot drift onto a
// different surface; it can only slide along the one it already describes,
// which is the whole point of jittering it.
// `tolerance` is how far the point may sit off the probe's plane and still be
// the same surface. It is deliberately *not* the probe's influence radius: a
// refined probe serves a small region but is free to move anywhere within the
// tile that spawned it, so matching it against its own narrow radius would
// reject its history every time it shifted by a pixel - which is every frame,
// because the seed it is elected relative to moves too.
fn probe_matches_surface(
    probe : SparseVoxelProbe, identity : vec2<u32>, position : vec3<f32>,
    normal : vec3<f32>, depth : f32, tolerance : f32) -> bool {
    if ((probe.identityState.z & SPARSE_VOXEL_PROBE_STATE_VALID) == 0u) {
        return false;
    }
    if (probe.identityState.x != identity.x) { return false; }
    if (dot(probe.normalRadius.xyz, normal) < settings.thresholds.x) {
        return false;
    }
    let depthScale = max(max(abs(depth), abs(probe.positionDepth.w)), 1.0);
    if (abs(depth - probe.positionDepth.w) / depthScale >
        settings.thresholds.y) { return false; }
    // Distance off the probe's plane. This is what the voxel id was standing in
    // for, measured directly.
    let planeDistance = abs(
        dot(position - probe.positionDepth.xyz, probe.normalRadius.xyz));
    return planeDistance <= max(tolerance, 1.0e-4);
}

// History for a refined probe. Its own index is allocated by a race and means
// nothing across frames, so the anchor is the tile: last frame's registry lists
// the refined probes this tile spawned, and the one describing the same surface
// is this probe's predecessor.
//
// Without this a refined probe is a fresh single-frame estimate every frame -
// 64 rays and no accumulation - which is precisely the noise refinement was
// added to remove. Refined probes serve the pixels with the most geometric
// variation, so they are the last place that can afford to have no history.
fn probe_refined_history(
    pixel : vec2<u32>, identity : vec2<u32>, position : vec3<f32>,
    normal : vec3<f32>, depth : f32, tolerance : f32) -> u32 {
    if (settings.probeCounts.z == 0u) { return 0xffffffffu; }
    let dimensions = max(settings.dimensionsTile.xy, vec2<u32>(1u));
    let tileSize = max(settings.dimensionsTile.z, 1u);
    let probeCounts = settings.probeCounts.xy;
    let motion = textureLoad(motionVectorTexture, vec2<i32>(pixel), 0).xy;
    let uv = (vec2<f32>(pixel) + vec2<f32>(0.5)) / vec2<f32>(dimensions);
    let historyUv = uv - motion;
    if (any(historyUv < vec2<f32>(0.0)) || any(historyUv >= vec2<f32>(1.0))) {
        return 0xffffffffu;
    }
    let historyPixel = vec2<u32>(historyUv * vec2<f32>(dimensions));
    let historyProbeCoord = vec2<i32>(min(
        historyPixel / tileSize, probeCounts - vec2<u32>(1u)));
    let maximumCoord = vec2<i32>(probeCounts) - vec2<i32>(1);
    var best = 0xffffffffu;
    var bestDistance = 1.0e30;
    for (var offsetY = -1; offsetY <= 1; offsetY += 1) {
        for (var offsetX = -1; offsetX <= 1; offsetX += 1) {
            let coord = vec2<u32>(clamp(
                historyProbeCoord + vec2<i32>(offsetX, offsetY),
                vec2<i32>(0), maximumCoord));
            let historyTileIndex = sparse_voxel_probe_index(
                coord, probeCounts);
            // The inverse role change is equally common: a face that owned the
            // tile last frame can become secondary without becoming a new
            // lighting sample.
            if (historyTileIndex < arrayLength(&previousProbes)) {
                let previous = previousProbes[historyTileIndex];
                if (probe_matches_surface(
                        previous, identity, position, normal, depth,
                        tolerance)) {
                    let separation = distance(
                        position, previous.positionDepth.xyz);
                    if (separation < bestDistance ||
                        (separation == bestDistance &&
                         historyTileIndex < best)) {
                        bestDistance = separation;
                        best = historyTileIndex;
                    }
                }
            }
            let base = historyTileIndex *
                SPARSE_VOXEL_PROBE_ADAPTIVE_TILE_STRIDE;
            if (base >= arrayLength(&previousAdaptiveTiles)) { continue; }
            let count = min(
                previousAdaptiveTiles[base],
                SPARSE_VOXEL_PROBE_ADAPTIVE_PER_TILE);
            for (var i = 0u; i < count; i += 1u) {
                let entry = base + 1u + i;
                if (entry >= arrayLength(&previousAdaptiveTiles)) { break; }
                let candidate = previousAdaptiveTiles[entry];
                if (candidate >= arrayLength(&previousProbes)) { continue; }
                let previous = previousProbes[candidate];
                if (!probe_matches_surface(
                        previous, identity, position, normal, depth,
                        tolerance)) {
                    continue;
                }
                let separation = distance(
                    position, previous.positionDepth.xyz);
                if (separation < bestDistance ||
                    (separation == bestDistance && candidate < best)) {
                    bestDistance = separation;
                    best = candidate;
                }
            }
        }
    }
    return best;
}

fn probe_make_record(
    pixel : vec2<u32>, footprintPixels : u32, frame : u32, extraState : u32,
    refined : bool) -> SparseVoxelProbe {
    let dimensions = max(settings.dimensionsTile.xy, vec2<u32>(1u));
    let pixelIndex = pixel.y * dimensions.x + pixel.x;
    let depth = linearDepth[pixelIndex];
    let position = probe_world_position(pixel, depth);
    let normal = probe_normalize(
        textureLoad(geometryNormalTexture, vec2<i32>(pixel), 0).xyz);
    let identity = probe_identity(vec2<i32>(pixel));
    var historyIndex = 0xffffffffu;
    // History always tolerates a whole tile of movement, whatever footprint the
    // probe itself serves.
    let tolerance = probe_radius(depth, settings.dimensionsTile.z);
    if (refined) {
        historyIndex = probe_refined_history(
            pixel, identity, position, normal, depth, tolerance);
    } else {
        historyIndex = probe_history_index(
            pixel, depth, position, normal, identity, tolerance);
    }
    var state = SPARSE_VOXEL_PROBE_STATE_VALID | extraState;
    if (historyIndex != 0xffffffffu) {
        state |= SPARSE_VOXEL_PROBE_STATE_REPROJECTED;
    }
    // How many consecutive frames this probe has described the same surface.
    // The resolve turns it into a blend weight, so a probe converges quickly
    // when it is new and then holds still - rather than sitting forever at the
    // noise floor a fixed blend weight implies.
    var sampleCount = 0u;
    if (historyIndex != 0xffffffffu) {
        sampleCount = min(
            previousProbes[historyIndex].identityState.w + 1u,
            SPARSE_VOXEL_PROBE_MAXIMUM_SAMPLES);
    }
    return SparseVoxelProbe(
        vec4<f32>(position, depth),
        vec4<f32>(normal, probe_radius(depth, footprintPixels)),
        vec4<u32>(identity.x, identity.y, state, sampleCount),
        vec4<u32>(pixel.x, pixel.y, frame, historyIndex));
}

// How badly a probe fails to represent a surface point, in units of the
// probe's own world footprint. Zero means the probe describes this point; the
// value grows with how far off the probe's plane the point sits.
//
// This is deliberately the same geometry the interpolation weights use. A
// pixel that refinement considers represented is exactly a pixel that
// interpolation will accept, so refinement spends its budget on the pixels
// that would otherwise have fallen through to the fallback.
fn probe_representation_error(
    probePosition : vec3<f32>, probeNormal : vec3<f32>, probeRadius : f32,
    probeDepth : f32, position : vec3<f32>, normal : vec3<f32>, depth : f32,
    view : vec3<f32>) -> f32 {
    let orientation = dot(probeNormal, normal);
    if (orientation < settings.thresholds.x) { return 1.0e6; }
    let depthScale = max(max(abs(depth), abs(probeDepth)), 1.0);
    if (abs(depth - probeDepth) / depthScale > settings.thresholds.y) {
        return 1.0e6;
    }
    // A tile on a surface seen at a grazing angle spans far more world space
    // than its perpendicular footprint, so the acceptance radius has to widen
    // with obliquity or every oblique surface reports as unrepresented and
    // refinement burns its whole budget on floors.
    let obliquity = clamp(abs(dot(normal, view)), 0.15, 1.0);
    let radius = max(
        probeRadius * max(settings.thresholds.w, 1.0e-3) / obliquity, 1.0e-4);
    return abs(dot(position - probePosition, probeNormal)) / radius;
}

@compute @workgroup_size(64, 1, 1)
fn resetSparseVoxelProbeAllocation(
    @builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x != 0u) { return; }
    // The histogram standing in the buffer is the previous frame's, complete.
    // Walk it worst-bin-first and find where the budget runs out: every bin
    // above the cutoff is affordable in full, the cutoff bin is affordable in
    // part, and everything below it is refused.
    //
    // Deriving the cutoff from the finished previous frame rather than the
    // current one is what keeps refinement a single pass. Demand is strongly
    // coherent frame to frame, so the estimate is good, and the alternative is
    // a prefix sum over every tile between two placement dispatches.
    let capacity = settings.probeCounts.w;
    var remaining = capacity;
    var cutoffBin = 0u;
    var cutoffShare = SPARSE_VOXEL_PROBE_ALLOCATION_SHARE_SCALE;
    for (var i = 0u; i < SPARSE_VOXEL_PROBE_PRIORITY_BINS; i += 1u) {
        let bin = SPARSE_VOXEL_PROBE_PRIORITY_BINS - 1u - i;
        let demand = atomicLoad(
            &allocation[SPARSE_VOXEL_PROBE_ALLOCATION_HISTOGRAM + bin]);
        if (demand <= remaining) {
            remaining -= demand;
            cutoffBin = bin;
            cutoffShare = SPARSE_VOXEL_PROBE_ALLOCATION_SHARE_SCALE;
            continue;
        }
        cutoffBin = bin;
        cutoffShare = remaining *
            SPARSE_VOXEL_PROBE_ALLOCATION_SHARE_SCALE / max(demand, 1u);
        break;
    }
    atomicStore(
        &allocation[SPARSE_VOXEL_PROBE_ALLOCATION_CUTOFF_BIN], cutoffBin);
    atomicStore(
        &allocation[SPARSE_VOXEL_PROBE_ALLOCATION_CUTOFF_SHARE], cutoffShare);
    atomicStore(&allocation[SPARSE_VOXEL_PROBE_ALLOCATION_COUNT], 0u);
    for (var bin = 0u; bin < SPARSE_VOXEL_PROBE_PRIORITY_BINS; bin += 1u) {
        atomicStore(
            &allocation[SPARSE_VOXEL_PROBE_ALLOCATION_HISTOGRAM + bin], 0u);
    }
}

// Overflow slots that refinement did not claim this frame still hold last
// frame's record. Ray generation and resolve key off the valid flag alone, so
// a stale record would emit rays for a probe that no longer exists and write
// its radiance into the atlas.
@compute @workgroup_size(64, 1, 1)
fn clearSparseVoxelProbeOverflow(
    @builtin(global_invocation_id) gid : vec3<u32>) {
    let capacity = settings.probeCounts.w;
    if (gid.x >= capacity) { return; }
    if (gid.x < atomicLoad(
            &allocation[SPARSE_VOXEL_PROBE_ALLOCATION_COUNT])) { return; }
    let uniformCount = settings.probeCounts.x * settings.probeCounts.y;
    let probeIndex = uniformCount + gid.x;
    if (probeIndex >= arrayLength(&probes)) { return; }
    probes[probeIndex] = SparseVoxelProbe(
        vec4<f32>(0.0), vec4<f32>(0.0, 1.0, 0.0, 0.0),
        vec4<u32>(0u, 0u, 0u, 0u),
        vec4<u32>(0u, 0u, settings.dimensionsTile.w, 0xffffffffu));
}

@compute @workgroup_size(64, 1, 1)
fn placeSparseVoxelProbes(
    @builtin(workgroup_id) workgroupId : vec3<u32>,
    @builtin(local_invocation_index) localIndex : u32) {
    let dimensions = max(settings.dimensionsTile.xy, vec2<u32>(1u));
    let tileSize = max(settings.dimensionsTile.z, 1u);
    let frame = settings.dimensionsTile.w;
    let probeCounts = settings.probeCounts.xy;
    let uniformCount = probeCounts.x * probeCounts.y;
    let probeIndex = workgroupId.x;
    if (probeIndex >= uniformCount) { return; }
    let probeCoord = vec2<u32>(
        probeIndex % probeCounts.x, probeIndex / probeCounts.x);
    let tileOrigin = probeCoord * tileSize;
    let registryBase = probeIndex * SPARSE_VOXEL_PROBE_ADAPTIVE_TILE_STRIDE;

    if (localIndex == 0u) {
        atomicStore(&bestScore, 0xffffffffu);
        atomicStore(&coveredCount, 0u);
        atomicStore(&tileSurfacePixels, 0u);
        atomicStore(&tileNormalTransitions, 0u);
        for (var face = 0u; face < 6u; face += 1u) {
            atomicStore(&faceCount[face], 0u);
            atomicStore(&faceCentroidX[face], 0u);
            atomicStore(&faceCentroidY[face], 0u);
            atomicStore(&faceScore[face], 0xffffffffu);
        }
        selectedFaceCount = 0u;
        refinePlaced = 0u;
        refineStarved = 0u;
    }
    workgroupBarrier();

    // Seed selection.
    //
    // Attempt zero is the four-frame cycle's position for this tile. The rest
    // are low-discrepancy fallbacks for tiles where that position is not
    // covered.
    //
    // The election prefers, above all else, a candidate lying on the *same
    // surface this tile's probe described last frame*. That preference is what
    // stops the tile flickering. A tile containing two surfaces has no single
    // right answer, and a purely stochastic seed answers differently every
    // frame: the probe measures the near surface, then the far one, then the
    // near one again, and the tile visibly alternates between two brightnesses.
    // It also destroys history, because a probe that changed surface cannot
    // reproject, so each of those alternating estimates is a raw 64-ray sample
    // with no accumulation behind it.
    //
    // Anchoring to last frame's surface makes the choice stable, which lets
    // history accumulate - and the surfaces the anchored probe now ignores are
    // exactly what refinement below exists to cover.
    var previousTileProbe = SparseVoxelProbe(
        vec4<f32>(0.0), vec4<f32>(0.0, 1.0, 0.0, 0.0), vec4<u32>(0u),
        vec4<u32>(0u));
    if (settings.probeCounts.z != 0u &&
        probeIndex < arrayLength(&previousProbes)) {
        previousTileProbe = previousProbes[probeIndex];
    }
    let phase = sparse_voxel_probe_tile_phase(probeCoord, frame);
    for (var attempt = localIndex; attempt < PROBE_PLACE_ATTEMPTS;
         attempt += PROBE_PLACE_GROUP) {
        var jitter = sparse_voxel_probe_cycle_offset(phase);
        if (attempt != 0u) {
            jitter = probe_hammersley(
                probe_pcg(frame) + probe_pcg(
                    tileOrigin.x +
                    probe_pcg(tileOrigin.y + probe_pcg(attempt))));
        }
        let candidate = tileOrigin +
            vec2<u32>(floor(vec2<f32>(f32(tileSize)) * jitter));
        let pixel = min(candidate, dimensions - vec2<u32>(1u));
        if (any(pixel >= dimensions)) { continue; }
        let pixelIndex = pixel.y * dimensions.x + pixel.x;
        if (pixelIndex >= arrayLength(&coverage) ||
            coverage[pixelIndex] == 0u ||
            pixelIndex >= arrayLength(&linearDepth)) { continue; }
        atomicAdd(&coveredCount, 1u);
        let depth = linearDepth[pixelIndex];
        let normal = probe_normalize(
            textureLoad(geometryNormalTexture, vec2<i32>(pixel), 0).xyz);
        let identity = probe_identity(vec2<i32>(pixel));
        let continues = probe_matches_surface(
            previousTileProbe, identity, probe_world_position(pixel, depth),
            normal, depth, probe_radius(depth, tileSize));
        // Surface continuity in the high bit, attempt order below it: a
        // matching candidate beats every non-matching one, and among equals the
        // earliest (lowest-discrepancy) attempt wins. Once the predecessor has
        // accumulated enough samples, leave attempt zero free for its exact
        // seed below. A static probe must eventually stop changing its receiver
        // or hard shadow visibility keeps injecting fresh variance forever.
        let settled = previousTileProbe.identityState.w >=
            PROBE_PLACE_STABLE_SAMPLES;
        let attemptRank = attempt + select(0u, 1u, settled);
        let packed = (select(1u, 0u, continues) << PROBE_PLACE_CONTINUES_SHIFT) |
            (attemptRank << PROBE_PLACE_ATTEMPT_SHIFT) |
            probe_place_pack_local(pixel - tileOrigin);
        atomicMin(&bestScore, packed);
    }
    // The previous frame's own seed pixel, offered as one more candidate.
    //
    // Anchoring above only works if some jittered attempt happens to land back
    // on last frame's surface. When none does, the tile is forced onto whatever
    // surface the attempts did hit - and because a probe's normal comes from
    // its seed pixel, and voxel faces are axis-aligned and therefore mutually
    // perpendicular, that flips the probe's normal by 90 degrees.
    //
    // `probe_representation_error` hard-rejects on normal, so one flip inverts
    // representability for *every pixel in the tile at once*: the whole region
    // is suddenly unrepresented, refinement floods it with probes, and next
    // frame the seed lands back and they vanish. That is a region changing
    // brightness in one frame, it is driven entirely by the placement jitter,
    // and it is why the reported dancing disappears when TAA is switched off.
    //
    // Re-offering last frame's pixel costs one candidate and removes the
    // forcing: the tile can still move to a new surface, but only when the old
    // one is genuinely gone rather than merely unsampled this frame. It is
    // During its learning window it is ranked below an ordinary continuing
    // candidate, so the probe still integrates the tile. Once settled it owns
    // the reserved first rank and the validated receiver becomes stationary.
    //
    // This used to be gated on `flags.x`, which is the *diagnostics* flag, so
    // the anchoring only ran while probe statistics were being collected -
    // exactly the configuration everything above was measured in, and never the
    // one anybody looks at. Interactively the flicker it was written to remove
    // was still fully present. It has its own flag now (`flags.y`).
    if (localIndex == 0u && settings.flags.y != 0u &&
        (previousTileProbe.identityState.z &
            SPARSE_VOXEL_PROBE_STATE_VALID) != 0u) {
        let pixel = min(
            previousTileProbe.pixelFrameHistory.xy, dimensions - vec2<u32>(1u));
        let pixelIndex = pixel.y * dimensions.x + pixel.x;
        if (pixelIndex < arrayLength(&coverage) &&
            coverage[pixelIndex] != 0u &&
            pixelIndex < arrayLength(&linearDepth) &&
            pixel.x >= tileOrigin.x && pixel.y >= tileOrigin.y &&
            pixel.x < tileOrigin.x + tileSize &&
            pixel.y < tileOrigin.y + tileSize) {
            let depth = linearDepth[pixelIndex];
            let normal = probe_normalize(
                textureLoad(geometryNormalTexture, vec2<i32>(pixel), 0).xyz);
            let identity = probe_identity(vec2<i32>(pixel));
            if (probe_matches_surface(
                    previousTileProbe, identity,
                    probe_world_position(pixel, depth), normal, depth,
                    probe_radius(depth, tileSize))) {
                atomicAdd(&coveredCount, 1u);
                // During the learning window this remains below continuing
                // jittered candidates, preserving the tile integration that
                // establishes a representative result. Afterwards it takes
                // the reserved rank zero and becomes the stable receiver for
                // this surface. A changed/disoccluded surface still fails the
                // predicate and immediately returns to placement.
                let settled = previousTileProbe.identityState.w >=
                    PROBE_PLACE_STABLE_SAMPLES;
                atomicMin(
                    &bestScore,
                    (select(1u, 0u, settled) <<
                        PROBE_PLACE_CONTINUES_SHIFT) |
                        probe_place_pack_local(pixel - tileOrigin));
            }
        }
    }
    workgroupBarrier();

    let covered = atomicLoad(&coveredCount);
    if (covered == 0u) {
        if (localIndex == 0u && probeIndex < arrayLength(&probes)) {
            // An uncovered tile still writes a record so the interpolation pass
            // can reject it without a separate validity buffer.
            probes[probeIndex] = SparseVoxelProbe(
                vec4<f32>(0.0), vec4<f32>(0.0, 1.0, 0.0, 0.0),
                vec4<u32>(0u, 0u, 0u, 0u),
                vec4<u32>(0u, 0u, frame, 0xffffffffu));
            if (registryBase < arrayLength(&adaptiveTiles)) {
                adaptiveTiles[registryBase] = 0u;
            }
        }
        return;
    }

    let winner = atomicLoad(&bestScore);
    let seedPixel = probe_place_unpack_pixel(winner, tileOrigin, dimensions);
    let seedPixelIndex = seedPixel.y * dimensions.x + seedPixel.x;
    let seedDepth = linearDepth[seedPixelIndex];

    // Measure how densely face orientation changes inside this tile. The
    // geometry normal remains unsmoothed specifically so this detects real
    // voxel-face complexity even though the material shading normal is filtered.
    let pixelsInTile = tileSize * tileSize;
    for (var i = localIndex; i < pixelsInTile; i += PROBE_PLACE_GROUP) {
        let local = vec2<u32>(i % tileSize, i / tileSize);
        let pixel = tileOrigin + local;
        if (any(pixel >= dimensions)) { continue; }
        let pixelIndex = pixel.y * dimensions.x + pixel.x;
        if (pixelIndex >= arrayLength(&coverage) || coverage[pixelIndex] == 0u) {
            continue;
        }
        atomicAdd(&tileSurfacePixels, 1u);
        let normal = probe_normalize(
            textureLoad(geometryNormalTexture, vec2<i32>(pixel), 0).xyz);
        let faceClass = probe_face_class(normal);
        atomicAdd(&faceCount[faceClass], 1u);
        atomicAdd(&faceCentroidX[faceClass], local.x);
        atomicAdd(&faceCentroidY[faceClass], local.y);
        let instance = textureLoad(instanceIdTexture, vec2<i32>(pixel), 0).x;
        let tileEnd = min(tileOrigin + vec2<u32>(tileSize), dimensions);
        for (var axis = 0u; axis < 2u; axis += 1u) {
            let offset = select(vec2<u32>(0u, 1u), vec2<u32>(1u, 0u), axis == 0u);
            let neighbour = pixel + offset;
            if (any(neighbour >= tileEnd)) { continue; }
            let neighbourIndex = neighbour.y * dimensions.x + neighbour.x;
            if (neighbourIndex >= arrayLength(&coverage) ||
                coverage[neighbourIndex] == 0u ||
                textureLoad(instanceIdTexture, vec2<i32>(neighbour), 0).x !=
                    instance) { continue; }
            let neighbourNormal = probe_normalize(textureLoad(
                geometryNormalTexture, vec2<i32>(neighbour), 0).xyz);
            if (dot(normal, neighbourNormal) < 0.5) {
                atomicAdd(&tileNormalTransitions, 1u);
            }
        }
    }
    workgroupBarrier();
    // Elect the pixel nearest each face cluster's centroid. Unlike the primary
    // stochastic seed this is stable under jitter while that face remains in
    // the tile, which lets probe_refined_history keep its radiance cache.
    if (settings.faceSettings.y != 0.0) {
        for (var i = localIndex; i < pixelsInTile; i += PROBE_PLACE_GROUP) {
            let local = vec2<u32>(i % tileSize, i / tileSize);
            let pixel = tileOrigin + local;
            if (any(pixel >= dimensions)) { continue; }
            let pixelIndex = pixel.y * dimensions.x + pixel.x;
            if (pixelIndex >= arrayLength(&coverage) ||
                coverage[pixelIndex] == 0u) { continue; }
            let normal = probe_normalize(
                textureLoad(geometryNormalTexture, vec2<i32>(pixel), 0).xyz);
            let faceClass = probe_face_class(normal);
            let count = atomicLoad(&faceCount[faceClass]);
            if (count == 0u) { continue; }
            let centroid = vec2<f32>(
                f32(atomicLoad(&faceCentroidX[faceClass])),
                f32(atomicLoad(&faceCentroidY[faceClass]))) / f32(count);
            let delta = vec2<f32>(local) - centroid;
            let rank = min(u32(dot(delta, delta) * 16.0), 0xffffu);
            atomicMin(
                &faceScore[faceClass],
                (rank << PROBE_PLACE_RANK_SHIFT) |
                    probe_place_pack_local(local));
        }
    }
    workgroupBarrier();
    let surfacePixels = atomicLoad(&tileSurfacePixels);
    let normalTransitions = atomicLoad(&tileNormalTransitions);
    // One clean edge is normally below this threshold; a checker of tiny voxel
    // faces is well above it. Keep the comparison integer and deterministic.
    let highNoiseTile = surfacePixels >= 4u &&
        normalTransitions * 4u >= surfacePixels;
    let seedNormal = probe_normalize(textureLoad(
        geometryNormalTexture, vec2<i32>(seedPixel), 0).xyz);
    let seedFace = probe_face_class(seedNormal);
    var hasSplitFace = false;
    if (settings.faceSettings.y != 0.0) {
        for (var face = 0u; face < 6u; face += 1u) {
            hasSplitFace = hasSplitFace ||
                (atomicLoad(&faceCount[face]) != 0u &&
                 probe_face_is_split(seedFace, face));
        }
    }
    let distanceDensityStride = sparse_voxel_probe_distance_stride(
        seedDepth, settings.distances.x, settings.distances.y,
        max(settings.flags.z, 1u));
    // Far planar surfaces are thinned. A tile with either dense normal noise or
    // one face beyond the configured split angle must remain independently
    // addressable; otherwise distance LOD would discard the tile before its
    // second/third face probes could be placed.
    let densityStride = select(
        distanceDensityStride, 1u, highNoiseTile || hasSplitFace);
    let densityAnchor = all((probeCoord % densityStride) == vec2<u32>(0u));
    if (!densityAnchor) {
        if (localIndex == 0u && probeIndex < arrayLength(&probes)) {
            probes[probeIndex] = SparseVoxelProbe(
                vec4<f32>(0.0), vec4<f32>(0.0, 1.0, 0.0, 0.0),
                vec4<u32>(0u, 0u, 0u, 0u),
                vec4<u32>(0u, 0u, frame, 0xffffffffu));
            if (registryBase < arrayLength(&adaptiveTiles)) {
                adaptiveTiles[registryBase] = 0u;
            }
        }
        return;
    }

    // The surviving probe represents the macro-tile, not just its original
    // 8x8 tile. Its larger world radius lets interpolation and sharing accept
    // it across the gap left by the probes this tier removed.
    let uniformProbe = probe_make_record(
        seedPixel, tileSize * densityStride, frame,
        sparse_voxel_probe_density_state(densityStride), false);
    if (localIndex == 0u && probeIndex < arrayLength(&probes)) {
        probes[probeIndex] = uniformProbe;
    }
    // Refinement would immediately repopulate the tiles distance LOD removed.
    // Far geometry deliberately relies on the wider probe and spatial sharing;
    // detailed near geometry keeps the existing adaptive descent unchanged.
    if (densityStride > 1u) {
        if (localIndex == 0u && registryBase < arrayLength(&adaptiveTiles)) {
            adaptiveTiles[registryBase] = 0u;
        }
        return;
    }

    let capacity = settings.probeCounts.w;
    // Reserve the two refinement lanes for distinct face clusters first. The
    // previous registry is consulted before current pixel counts, so a face
    // that was cached last frame keeps its lane while it remains visible even
    // if raster jitter changes which face owns the primary probe.
    if (localIndex == 0u && settings.faceSettings.y != 0.0 && capacity > 0u) {
        let primaryFace = (uniformProbe.identityState.x >> 29u) & 7u;
        if (registryBase < arrayLength(&previousAdaptiveTiles)) {
            let previousCount = min(
                previousAdaptiveTiles[registryBase],
                SPARSE_VOXEL_PROBE_ADAPTIVE_PER_TILE);
            for (var previousSlot = 0u;
                 previousSlot < previousCount &&
                     selectedFaceCount < SPARSE_VOXEL_PROBE_ADAPTIVE_PER_TILE;
                 previousSlot += 1u) {
                let entry = registryBase + 1u + previousSlot;
                if (entry >= arrayLength(&previousAdaptiveTiles)) { break; }
                let previousIndex = previousAdaptiveTiles[entry];
                if (previousIndex >= arrayLength(&previousProbes)) { continue; }
                let previous = previousProbes[previousIndex];
                if ((previous.identityState.z &
                    SPARSE_VOXEL_PROBE_STATE_VALID) == 0u) { continue; }
                let face = (previous.identityState.x >> 29u) & 7u;
                if (face >= 6u || atomicLoad(&faceCount[face]) == 0u ||
                    !probe_face_is_split(primaryFace, face)) { continue; }
                var duplicate = false;
                for (var selected = 0u; selected < selectedFaceCount;
                     selected += 1u) {
                    duplicate = duplicate ||
                        selectedFaceClasses[selected] == face;
                }
                if (!duplicate) {
                    selectedFaceClasses[selectedFaceCount] = face;
                    selectedFaceCount += 1u;
                }
            }
        }
        // Fill any unclaimed lane from the largest currently visible face
        // cluster. Ties resolve by face class, making the result deterministic.
        loop {
            if (selectedFaceCount >= SPARSE_VOXEL_PROBE_ADAPTIVE_PER_TILE) {
                break;
            }
            var bestFace = 0xffffffffu;
            var bestCount = 0u;
            for (var face = 0u; face < 6u; face += 1u) {
                if (!probe_face_is_split(primaryFace, face)) { continue; }
                var duplicate = false;
                for (var selected = 0u; selected < selectedFaceCount;
                     selected += 1u) {
                    duplicate = duplicate ||
                        selectedFaceClasses[selected] == face;
                }
                if (duplicate) { continue; }
                let count = atomicLoad(&faceCount[face]);
                if (count > bestCount ||
                    (count == bestCount && count > 0u && face < bestFace)) {
                    bestCount = count;
                    bestFace = face;
                }
            }
            if (bestFace == 0xffffffffu || bestCount == 0u) { break; }
            selectedFaceClasses[selectedFaceCount] = bestFace;
            selectedFaceCount += 1u;
        }

        for (var selected = 0u; selected < selectedFaceCount;
             selected += 1u) {
            let face = selectedFaceClasses[selected];
            let score = atomicLoad(&faceScore[face]);
            if (score == 0xffffffffu) { continue; }
            let slot = atomicAdd(
                &allocation[SPARSE_VOXEL_PROBE_ALLOCATION_COUNT], 1u);
            if (slot >= capacity) {
                atomicSub(
                    &allocation[SPARSE_VOXEL_PROBE_ALLOCATION_COUNT], 1u);
                refineStarved = 1u;
                break;
            }
            let pixel = probe_place_unpack_pixel(score, tileOrigin, dimensions);
            let footprint = min(
                max(u32(ceil(sqrt(f32(atomicLoad(&faceCount[face]))))), 1u),
                tileSize);
            let refined = probe_make_record(
                pixel, footprint, frame,
                SPARSE_VOXEL_PROBE_STATE_ADAPTIVE, true);
            let refinedIndex = uniformCount + slot;
            if (refinedIndex < arrayLength(&probes)) {
                probes[refinedIndex] = refined;
                refinePosition[refinePlaced] = vec4<f32>(
                    refined.positionDepth.xyz, refined.normalRadius.w);
                refineNormal[refinePlaced] = vec4<f32>(
                    refined.normalRadius.xyz, refined.positionDepth.w);
                if (registryBase + 1u + refinePlaced <
                    arrayLength(&adaptiveTiles)) {
                    adaptiveTiles[registryBase + 1u + refinePlaced] =
                        refinedIndex;
                }
                refinePlaced += 1u;
            }
        }
    }
    workgroupBarrier();

    // Refinement: a demand-driven descent of a quadtree over the tile.
    //
    // Each level subdivides into four times as many regions, and a region only
    // gets a probe if the probes above it - the tile probe and everything
    // placed at shallower levels - still fail to represent its pixels. A flat
    // tile therefore stops at the first level having placed nothing, while a
    // tile containing a railing against a distant wall keeps subdividing until
    // each distinct surface has a probe describing it.
    //
    // The representative is the unrepresented pixel nearest its region's
    // *centroid*, not the one with the worst error. The worst-error pixel is
    // almost always on a silhouette - it is worst precisely because it is
    // extreme - which makes it both a poor description of the region's bulk and
    // a position that jumps between frames as the tile probe moves. The
    // centroid moves smoothly, so the refined probe stays put and keeps its
    // history.
    let cutoffBin = atomicLoad(&allocation[SPARSE_VOXEL_PROBE_ALLOCATION_CUTOFF_BIN]);
    let cutoffShare = atomicLoad(&allocation[SPARSE_VOXEL_PROBE_ALLOCATION_CUTOFF_SHARE]);

    for (var level = 1u; level <= SPARSE_VOXEL_PROBE_ADAPTIVE_LEVELS;
         level += 1u) {
        let regionsPerAxis = 1u << level;
        let regions = regionsPerAxis * regionsPerAxis;
        let regionSize = max(tileSize >> level, 1u);

        workgroupBarrier();
        if (localIndex < regions) {
            atomicStore(&regionCount[localIndex], 0u);
            atomicStore(&regionCentroidX[localIndex], 0u);
            atomicStore(&regionCentroidY[localIndex], 0u);
            atomicStore(&regionError[localIndex], 0u);
            atomicStore(&regionScore[localIndex], 0xffffffffu);
        }
        workgroupBarrier();
        if (capacity == 0u) { break; }

        // Gather: which pixels remain unrepresented, and where they sit.
        let placed = refinePlaced;
        for (var i = localIndex; i < pixelsInTile; i += PROBE_PLACE_GROUP) {
            let local = vec2<u32>(i % tileSize, i / tileSize);
            let pixel = tileOrigin + local;
            if (any(pixel >= dimensions)) { continue; }
            let pixelIndex = pixel.y * dimensions.x + pixel.x;
            if (pixelIndex >= arrayLength(&coverage) ||
                coverage[pixelIndex] == 0u ||
                pixelIndex >= arrayLength(&linearDepth)) { continue; }
            let depth = linearDepth[pixelIndex];
            let position = probe_world_position(pixel, depth);
            let normal = probe_normalize(
                textureLoad(geometryNormalTexture, vec2<i32>(pixel), 0).xyz);
            let view = probe_view_direction(pixel);
            var error = probe_representation_error(
                uniformProbe.positionDepth.xyz, uniformProbe.normalRadius.xyz,
                uniformProbe.normalRadius.w, uniformProbe.positionDepth.w,
                position, normal, depth, view);
            for (var j = 0u; j < placed; j += 1u) {
                error = min(error, probe_representation_error(
                    refinePosition[j].xyz, refineNormal[j].xyz,
                    refinePosition[j].w, refineNormal[j].w,
                    position, normal, depth, view));
            }
            if (error <= 1.0) { continue; }
            if (settings.flags.x != 0u && level == 1u) {
                atomicAdd(
                    &diagnostics[
                        probe_place_diagnostic_region(tileOrigin, dimensions) +
                        PROBE_PLACE_DIAGNOSTIC_UNREPRESENTED],
                    1u);
            }
            let region = (local.x / regionSize) +
                (local.y / regionSize) * regionsPerAxis;
            if (region >= regions) { continue; }
            atomicAdd(&regionCount[region], 1u);
            atomicAdd(&regionCentroidX[region], local.x);
            atomicAdd(&regionCentroidY[region], local.y);
            atomicMax(&regionError[region], u32(min(error, 1.0e6) * 16.0));
        }
        workgroupBarrier();

        // Elect: the unrepresented pixel nearest its region's centroid.
        for (var i = localIndex; i < pixelsInTile; i += PROBE_PLACE_GROUP) {
            let local = vec2<u32>(i % tileSize, i / tileSize);
            let pixel = tileOrigin + local;
            if (any(pixel >= dimensions)) { continue; }
            let pixelIndex = pixel.y * dimensions.x + pixel.x;
            if (pixelIndex >= arrayLength(&coverage) ||
                coverage[pixelIndex] == 0u ||
                pixelIndex >= arrayLength(&linearDepth)) { continue; }
            let region = (local.x / regionSize) +
                (local.y / regionSize) * regionsPerAxis;
            if (region >= regions) { continue; }
            let count = atomicLoad(&regionCount[region]);
            if (count == 0u) { continue; }
            let depth = linearDepth[pixelIndex];
            let position = probe_world_position(pixel, depth);
            let normal = probe_normalize(
                textureLoad(geometryNormalTexture, vec2<i32>(pixel), 0).xyz);
            let view = probe_view_direction(pixel);
            var error = probe_representation_error(
                uniformProbe.positionDepth.xyz, uniformProbe.normalRadius.xyz,
                uniformProbe.normalRadius.w, uniformProbe.positionDepth.w,
                position, normal, depth, view);
            for (var j = 0u; j < placed; j += 1u) {
                error = min(error, probe_representation_error(
                    refinePosition[j].xyz, refineNormal[j].xyz,
                    refinePosition[j].w, refineNormal[j].w,
                    position, normal, depth, view));
            }
            if (error <= 1.0) { continue; }
            let centroid = vec2<f32>(
                f32(atomicLoad(&regionCentroidX[region])),
                f32(atomicLoad(&regionCentroidY[region]))) / f32(count);
            let offset = vec2<f32>(local) - centroid;
            let rank = min(u32(sqrt(dot(offset, offset)) * 16.0), 1023u);
            atomicMin(
                &regionScore[region],
                (rank << PROBE_PLACE_RANK_SHIFT) |
                    probe_place_pack_local(local));
        }
        workgroupBarrier();

        // Commit. Serialised on one lane so the registry order is
        // deterministic, which is what lets the next frame match these probes
        // back to their surfaces.
        if (localIndex == 0u) {
            for (var region = 0u; region < regions; region += 1u) {
                if (refinePlaced >= SPARSE_VOXEL_PROBE_ADAPTIVE_PER_TILE) {
                    break;
                }
                let count = atomicLoad(&regionCount[region]);
                if (count == 0u) { continue; }
                let candidate = atomicLoad(&regionScore[region]);
                if (candidate == 0xffffffffu) { continue; }

                // Budget arbitration, globally ordered by how badly the region
                // is misrepresented.
                //
                // The cutoff bin comes from the previous frame's demand
                // histogram, so the budget goes to the worst regions on the
                // screen rather than to whichever tile the GPU scheduled first.
                // Within the marginal bin acceptance is a deterministic hash,
                // which keeps the choice spatially uniform and - because the
                // hash excludes the frame index - stable from frame to frame.
                let error = f32(atomicLoad(&regionError[region])) / 16.0;
                var bin = sparse_voxel_probe_priority_bin(error, count);
                // Dense normal transitions mean this region contains many tiny
                // faces rather than one ordinary discontinuity. Promote it in
                // the global competition so overflow capacity follows visible
                // shading noise instead of GPU workgroup execution order.
                bin = min(
                    bin + select(0u, 2u, highNoiseTile),
                    SPARSE_VOXEL_PROBE_PRIORITY_BINS - 1u);
                let candidatePixel = probe_place_unpack_pixel(
                    candidate, tileOrigin, dimensions);
                // Record all demand for the next frame's cutoff, including
                // candidates this frame cannot afford. The old code calculated
                // `bin`, `cutoffBin`, and `cutoffShare` but never used any of
                // them, leaving allocation first-come in practice.
                atomicAdd(
                    &allocation[SPARSE_VOXEL_PROBE_ALLOCATION_HISTOGRAM + bin],
                    1u);
                let marginalHash = probe_pcg(
                    candidatePixel.x ^ probe_pcg(
                        candidatePixel.y ^ probe_pcg(region + level * 64u)));
                let accepted = bin > cutoffBin ||
                    (bin == cutoffBin &&
                     marginalHash % SPARSE_VOXEL_PROBE_ALLOCATION_SHARE_SCALE <
                        cutoffShare);
                if (!accepted) {
                    refineStarved = 1u;
                    continue;
                }
                let slot = atomicAdd(
                    &allocation[SPARSE_VOXEL_PROBE_ALLOCATION_COUNT], 1u);
                if (slot >= capacity) {
                    atomicSub(
                        &allocation[SPARSE_VOXEL_PROBE_ALLOCATION_COUNT], 1u);
                    refineStarved = 1u;
                    continue;
                }
                let pixel = candidatePixel;
                // The footprint is the square root of the pixels this probe has
                // to serve, bounded by its region: a probe covering four pixels
                // claims a two-pixel radius, not a whole tile's worth.
                let footprint = min(
                    max(u32(ceil(sqrt(f32(count)))), 1u), regionSize);
                let refined = probe_make_record(
                    pixel, footprint, frame,
                    SPARSE_VOXEL_PROBE_STATE_ADAPTIVE, true);
                let refinedIndex = uniformCount + slot;
                if (refinedIndex < arrayLength(&probes)) {
                    probes[refinedIndex] = refined;
                    refinePosition[refinePlaced] = vec4<f32>(
                        refined.positionDepth.xyz, refined.normalRadius.w);
                    refineNormal[refinePlaced] = vec4<f32>(
                        refined.normalRadius.xyz, refined.positionDepth.w);
                    if (registryBase + 1u + refinePlaced <
                        arrayLength(&adaptiveTiles)) {
                        adaptiveTiles[registryBase + 1u + refinePlaced] =
                            refinedIndex;
                    }
                    refinePlaced = refinePlaced + 1u;
                }
            }
        }
        workgroupBarrier();
        if (refinePlaced >= SPARSE_VOXEL_PROBE_ADAPTIVE_PER_TILE) { break; }
        // A level that could not be afforded ends the descent. Subdividing it
        // would generate four candidates covering the same pixels the refused
        // one would have, which inflates the demand histogram, raises the
        // cutoff, and starves the shallower levels that were affordable.
        if (refineStarved != 0u) { break; }
    }

    workgroupBarrier();
    if (localIndex == 0u && registryBase < arrayLength(&adaptiveTiles)) {
        adaptiveTiles[registryBase] = refinePlaced;
    }
    if (localIndex == 0u && settings.probeCounts.z != 0u) {
        let base = probe_place_diagnostic_region(tileOrigin, dimensions);
        atomicAdd(&diagnostics[base + PROBE_PLACE_DIAGNOSTIC_TILES], 1u);
        atomicAdd(
            &diagnostics[base + PROBE_PLACE_DIAGNOSTIC_COVERED],
            atomicLoad(&coveredCount));
        atomicAdd(
            &diagnostics[base + PROBE_PLACE_DIAGNOSTIC_PLACED], refinePlaced);
        atomicAdd(
            &diagnostics[base + PROBE_PLACE_DIAGNOSTIC_STARVED],
            select(0u, 1u, refineStarved != 0u));
    }
}
