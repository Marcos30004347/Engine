#include "sparse_voxel_types.wgsl"
#include "sparse_voxel_scene_ray_types.wgsl"

// Current-frame screen-space visibility for the near field skipped by the
// world tracer's conservative trace-cell origin bias.
//
// The old implementation sampled 64 isolated world positions. A complete
// screen-space blocker could lie between two samples, so increasing its depth
// tolerance did not make the trace reliable. This implementation follows the
// useful core of Widmer et al. (HPG 2015): a min/max depth quadtree rejects
// empty spans and full-resolution leaves are intersected as geometric planes.
// A finite depth slab is used only to close silhouettes and plane-parallel
// cases, where a single-layer depth buffer cannot represent the side face.

struct SparseVoxelSceneScreenTraceSettings {
    // x width, y height, z ray count, w maximum full-resolution leaf visits.
    viewport : vec4<u32>,
    // x depth-bounds mip count, w packs reverse Z in bit 0.
    controls : vec4<u32>,
    // x slab thickness multiplier, y maximum world distance,
    // z enabled, w receiver start offset in world pixels.
    traceParams : vec4<f32>,
    // viewDepth = y / (deviceDepth + x). z is the minimum world-space
    // occluder thickness (normally one receiver trace cell) and w its
    // maximum; a non-positive w leaves the slab unbounded.
    depthConversion : vec4<f32>,
}

struct SparseVoxelScreenDepthBoundsBuildSettings {
    destinationLevel : u32,
    _padding : vec3<u32>,
}

@group(0) @binding(0) var<uniform> settings : SparseVoxelSceneScreenTraceSettings;
@group(0) @binding(1) var sceneDepth : texture_depth_2d;
@group(0) @binding(2) var geometryNormal : texture_2d<f32>;
@group(0) @binding(3) var<storage, read> rays : array<SparseVoxelSceneRay>;
@group(0) @binding(4) var<storage, read_write> hits : array<SparseVoxelSceneRayHit>;
@group(0) @binding(5) var<storage, read> activeRayIndices : array<u32>;
@group(0) @binding(6) var<storage, read> activeRayState : array<u32>;
@group(0) @binding(7) var<uniform> raster : SparseVoxelRasterUniforms;
// Every mip is packed consecutively. A node stores front/back device depth in
// a projection-independent metric that always increases away from the camera.
// (-1, -1) is empty.
@group(0) @binding(8) var<storage, read_write> depthBounds : array<vec2<f32>>;
// One immutable instance per reduction dispatch. Using a single reduction
// entry point avoids a MoltenVK/SPIRV-Cross argument-buffer translation bug
// seen when the module contains entry points for the higher mip levels.
@group(0) @binding(9) var<uniform> depthBoundsBuildSettings :
    SparseVoxelScreenDepthBoundsBuildSettings;
@group(0) @binding(10) var<storage, read_write> screenTraceStatistics :
    array<atomic<u32>>;
@group(0) @binding(11) var instanceIdTexture : texture_2d<u32>;
@group(0) @binding(12) var voxelIdTexture : texture_2d<u32>;

const SCREEN_TRACE_MAX_MIPS : u32 = 16u;
const SCREEN_TRACE_MAX_VISITS : u32 = 1024u;
const SCREEN_TRACE_STAT_ACTIVE_RAYS : u32 = 0u;
const SCREEN_TRACE_STAT_NODE_VISITS : u32 = 1u;
const SCREEN_TRACE_STAT_LEAF_VISITS : u32 = 2u;
const SCREEN_TRACE_STAT_MAX_LEAF_VISITS : u32 = 3u;
const SCREEN_TRACE_STAT_HITS : u32 = 4u;
const SCREEN_TRACE_STAT_MISS_TMAX : u32 = 5u;
const SCREEN_TRACE_STAT_ESCAPED_SCENE : u32 = 6u;
const SCREEN_TRACE_STAT_SHORT_SEGMENTS : u32 = 7u;
const SCREEN_TRACE_STAT_PLANE_HITS : u32 = 8u;
const SCREEN_TRACE_STAT_SLAB_HITS : u32 = 9u;
const SCREEN_TRACE_STAT_RECEIVER_ID_REJECTS : u32 = 10u;
const SCREEN_TRACE_STAT_RECEIVER_SHEET_REJECTS : u32 = 11u;

fn screen_trace_dimensions() -> vec2<u32> {
    return max(settings.viewport.xy, vec2<u32>(1u));
}

fn screen_trace_reverse_z() -> bool {
    return (settings.controls.w & 1u) != 0u;
}

fn screen_trace_statistics_enabled() -> bool {
    return settings.controls.y != 0u && arrayLength(&screenTraceStatistics) >= 12u;
}

fn screen_trace_stat_add(index : u32, value : u32) {
    if (screen_trace_statistics_enabled()) {
        atomicAdd(&screenTraceStatistics[index], value);
    }
}

fn screen_trace_stat_terminal(status : u32, leafVisits : u32,
                              nodeVisits : u32, shortSegment : bool) {
    if (!screen_trace_statistics_enabled()) { return; }
    atomicAdd(&screenTraceStatistics[SCREEN_TRACE_STAT_NODE_VISITS], nodeVisits);
    atomicAdd(&screenTraceStatistics[SCREEN_TRACE_STAT_LEAF_VISITS], leafVisits);
    atomicMax(&screenTraceStatistics[SCREEN_TRACE_STAT_MAX_LEAF_VISITS], leafVisits);
    if (shortSegment) {
        atomicAdd(&screenTraceStatistics[SCREEN_TRACE_STAT_SHORT_SEGMENTS], 1u);
    }
    if (status == SPARSE_VOXEL_SCENE_RAY_STATUS_HIT) {
        atomicAdd(&screenTraceStatistics[SCREEN_TRACE_STAT_HITS], 1u);
    } else if (status == SPARSE_VOXEL_SCENE_RAY_STATUS_MISS_TMAX) {
        atomicAdd(&screenTraceStatistics[SCREEN_TRACE_STAT_MISS_TMAX], 1u);
    } else if (status == SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_SCENE) {
        atomicAdd(&screenTraceStatistics[SCREEN_TRACE_STAT_ESCAPED_SCENE], 1u);
    }
}

fn screen_trace_background(deviceDepth : f32) -> bool {
    return select(
        deviceDepth >= 0.999999,
        deviceDepth <= raster.gbufferClearDepth,
        screen_trace_reverse_z());
}

fn screen_trace_depth_metric(deviceDepth : f32) -> f32 {
    return select(deviceDepth, 1.0 - deviceDepth, screen_trace_reverse_z());
}

fn screen_trace_metric_device_depth(metric : f32) -> f32 {
    return select(metric, 1.0 - metric, screen_trace_reverse_z());
}

fn screen_trace_view_depth(deviceDepth : f32) -> f32 {
    return settings.depthConversion.y /
        max(deviceDepth + settings.depthConversion.x, 1.0e-8);
}

fn screen_trace_mip_dimensions(level : u32) -> vec2<u32> {
    var dimensions = screen_trace_dimensions();
    for (var mip = 0u; mip < level; mip++) {
        dimensions = max((dimensions + vec2<u32>(1u)) / vec2<u32>(2u),
                         vec2<u32>(1u));
    }
    return dimensions;
}

fn screen_trace_mip_offset(level : u32) -> u32 {
    var dimensions = screen_trace_dimensions();
    var offset = 0u;
    for (var mip = 0u; mip < level; mip++) {
        offset += dimensions.x * dimensions.y;
        dimensions = max((dimensions + vec2<u32>(1u)) / vec2<u32>(2u),
                         vec2<u32>(1u));
    }
    return offset;
}

fn screen_trace_bounds_index(pixel : vec2<u32>, level : u32) -> u32 {
    let dimensions = screen_trace_mip_dimensions(level);
    return screen_trace_mip_offset(level) + pixel.y * dimensions.x + pixel.x;
}

@compute @workgroup_size(8, 8, 1)
fn initializeSparseVoxelScreenDepthBounds(
    @builtin(global_invocation_id) gid : vec3<u32>) {
    let dimensions = screen_trace_dimensions();
    if (any(gid.xy >= dimensions)) { return; }
    let index = gid.y * dimensions.x + gid.x;
    let deviceDepth = textureLoad(sceneDepth, vec2<i32>(gid.xy), 0);
    if (screen_trace_background(deviceDepth)) {
        depthBounds[index] = vec2<f32>(-1.0);
    } else {
        let metric = screen_trace_depth_metric(deviceDepth);
        depthBounds[index] = vec2<f32>(metric);
    }
}

fn reduce_sparse_voxel_screen_depth_bounds(
    destination : vec2<u32>, destinationLevel : u32) {
    let destinationDimensions = screen_trace_mip_dimensions(destinationLevel);
    if (any(destination >= destinationDimensions)) { return; }
    let sourceLevel = destinationLevel - 1u;
    let sourceDimensions = screen_trace_mip_dimensions(sourceLevel);
    let sourceBase = destination * vec2<u32>(2u);
    var combined = vec2<f32>(1.0e30, -1.0);
    for (var childY = 0u; childY < 2u; childY++) {
        for (var childX = 0u; childX < 2u; childX++) {
            let child = sourceBase + vec2<u32>(childX, childY);
            if (any(child >= sourceDimensions)) { continue; }
            let bounds = depthBounds[
                screen_trace_bounds_index(child, sourceLevel)];
            if (bounds.x < 0.0) { continue; }
            combined.x = min(combined.x, bounds.x);
            combined.y = max(combined.y, bounds.y);
        }
    }
    if (combined.y < 0.0) { combined = vec2<f32>(-1.0); }
    depthBounds[screen_trace_bounds_index(destination, destinationLevel)] =
        combined;
}

@compute @workgroup_size(8, 8, 1)
fn reduceSparseVoxelScreenDepthBounds(
    @builtin(global_invocation_id) id : vec3<u32>) {
    reduce_sparse_voxel_screen_depth_bounds(
        id.xy, depthBoundsBuildSettings.destinationLevel);
}

fn screen_trace_camera_direction(pixel : vec2<f32>) -> vec3<f32> {
    return vec3<f32>(
        (pixel.x + 0.5 - raster.focalPrincipal.z) /
            max(raster.focalPrincipal.x, 1.0e-6),
        (pixel.y + 0.5 - raster.focalPrincipal.w) /
            max(raster.focalPrincipal.y, 1.0e-6),
        1.0);
}

struct ScreenTraceSurface {
    position : vec3<f32>,
    // Camera-relative, but rotated into world axes. Plane comparisons use
    // this value so nearby points never subtract two ~260 km f32 positions.
    relativePosition : vec3<f32>,
    normal : vec3<f32>,
    viewDepth : f32,
    valid : bool,
}

fn screen_trace_surface(pixel : vec2<i32>) -> ScreenTraceSurface {
    let dimensions = vec2<i32>(screen_trace_dimensions());
    if (any(pixel < vec2<i32>(0)) || any(pixel >= dimensions)) {
        return ScreenTraceSurface(
            vec3<f32>(0.0), vec3<f32>(0.0), vec3<f32>(0.0), 0.0, false);
    }
    let deviceDepth = textureLoad(sceneDepth, pixel, 0);
    if (screen_trace_background(deviceDepth)) {
        return ScreenTraceSurface(
            vec3<f32>(0.0), vec3<f32>(0.0), vec3<f32>(0.0), 0.0, false);
    }
    let cameraDirection = screen_trace_camera_direction(vec2<f32>(pixel));
    let viewDepth = screen_trace_view_depth(deviceDepth);
    let relativePosition =
        (raster.cameraToWorld *
            vec4<f32>(cameraDirection * viewDepth, 0.0)).xyz;
    let position = raster.cameraWorldPositionNear.xyz + relativePosition;
    let rawNormal = textureLoad(geometryNormal, pixel, 0).xyz;
    let normalLength = length(rawNormal);
    let normal = select(vec3<f32>(0.0), rawNormal / max(normalLength, 1.0e-8),
                        normalLength > 1.0e-6);
    return ScreenTraceSurface(
        position, relativePosition, normal, viewDepth,
        normalLength > 1.0e-6);
}

struct ScreenTraceProjection {
    pixel : vec2<f32>,
    clipW : f32,
    valid : bool,
}

fn screen_trace_project(position : vec3<f32>) -> ScreenTraceProjection {
    let clip = raster.worldToClip * vec4<f32>(position, 1.0);
    if (clip.w <= 1.0e-6) {
        return ScreenTraceProjection(vec2<f32>(0.0), clip.w, false);
    }
    let ndc = clip.xyz / clip.w;
    let uv = vec2<f32>(ndc.x * 0.5 + 0.5,
                       1.0 - (ndc.y * 0.5 + 0.5));
    return ScreenTraceProjection(
        uv * vec2<f32>(screen_trace_dimensions()), clip.w,
        ndc.z >= 0.0 && ndc.z <= 1.0);
}

fn screen_trace_pixel_size(viewDepth : f32) -> f32 {
    return max(viewDepth / max(raster.focalPrincipal.x, 1.0e-6), 1.0e-6);
}

fn screen_trace_clip_test(
    p : f32, q : f32, interval : ptr<function, vec2<f32>>) -> bool {
    if (abs(p) < 1.0e-8) { return q >= 0.0; }
    let ratio = q / p;
    if (p < 0.0) {
        (*interval).x = max((*interval).x, ratio);
    } else {
        (*interval).y = min((*interval).y, ratio);
    }
    return (*interval).x <= (*interval).y;
}

fn screen_trace_clip_segment(
    origin : vec2<f32>, delta : vec2<f32>, dimensions : vec2<f32>) -> vec2<f32> {
    var interval = vec2<f32>(0.0, 1.0);
    let upper = dimensions - vec2<f32>(1.0e-3);
    if (!screen_trace_clip_test(-delta.x, origin.x, &interval) ||
        !screen_trace_clip_test(delta.x, upper.x - origin.x, &interval) ||
        !screen_trace_clip_test(-delta.y, origin.y, &interval) ||
        !screen_trace_clip_test(delta.y, upper.y - origin.y, &interval)) {
        return vec2<f32>(1.0, 0.0);
    }
    return interval;
}

// Convert the linear parameter of the projected line back to the original
// world segment. This makes cell entry/exit depths agree with the 3D ray.
fn screen_trace_world_fraction(
    screenFraction : f32, startW : f32, endW : f32) -> f32 {
    let denominator =
        (1.0 - screenFraction) * endW + screenFraction * startW;
    if (abs(denominator) < 1.0e-8) { return screenFraction; }
    return clamp(screenFraction * startW / denominator, 0.0, 1.0);
}

fn screen_trace_node_exit(
    segmentOrigin : vec2<f32>, segmentDelta : vec2<f32>,
    node : vec2<u32>, level : u32, current : f32, end : f32) -> f32 {
    let cellSize = f32(1u << level);
    var result = end;
    if (segmentDelta.x > 1.0e-8) {
        result = min(result,
            (f32(node.x + 1u) * cellSize - segmentOrigin.x) / segmentDelta.x);
    } else if (segmentDelta.x < -1.0e-8) {
        result = min(result,
            (f32(node.x) * cellSize - segmentOrigin.x) / segmentDelta.x);
    }
    if (segmentDelta.y > 1.0e-8) {
        result = min(result,
            (f32(node.y + 1u) * cellSize - segmentOrigin.y) / segmentDelta.y);
    } else if (segmentDelta.y < -1.0e-8) {
        result = min(result,
            (f32(node.y) * cellSize - segmentOrigin.y) / segmentDelta.y);
    }
    return clamp(max(result, current), current, end);
}

fn screen_trace_discontinuous(pixel : vec2<i32>, surfaceDepth : f32) -> bool {
    let dimensions = vec2<i32>(screen_trace_dimensions());
    let threshold = max(screen_trace_pixel_size(surfaceDepth) * 4.0, 1.0e-4);
    let offsets = array<vec2<i32>, 4>(
        vec2<i32>(-1, 0), vec2<i32>(1, 0),
        vec2<i32>(0, -1), vec2<i32>(0, 1));
    for (var index = 0u; index < 4u; index++) {
        let neighbour = pixel + offsets[index];
        if (any(neighbour < vec2<i32>(0)) || any(neighbour >= dimensions)) {
            return true;
        }
        let depth = textureLoad(sceneDepth, neighbour, 0);
        if (screen_trace_background(depth) ||
            abs(screen_trace_view_depth(depth) - surfaceDepth) > threshold) {
            return true;
        }
    }
    return false;
}

// How deep behind a depth-buffer surface a ray still counts as occluded.
//
// The buffer records a front face and says nothing about how far the surface
// extends behind it, so the leaf test has to assume a finite slab. Scaling
// that slab by the traversed interval alone makes it metres deep for a grazing
// ray - such a ray crosses one cell over a long world distance - and every ray
// passing well behind the surface then reads as occluded. Those are false
// direct shadows: the world trace disagrees with them, and the disagreement
// appears and disappears as the occluder leaves the screen. Bound the slab by
// a world-space ceiling, which is the depth real geometry has near a receiver.
fn screen_trace_thickness(intervalLength : f32, bias : f32) -> f32 {
    let minimumThickness =
        max(bias * 2.0, max(settings.depthConversion.z, 0.0));
    let maximumThickness = select(
        1.0e30,
        max(settings.depthConversion.w, minimumThickness),
        settings.depthConversion.w > 0.0);
    return clamp(
        intervalLength * max(settings.traceParams.x, 0.0),
        minimumThickness, maximumThickness);
}

fn screen_trace_record_hit(
    ray : SparseVoxelSceneRay, rayIndex : u32, position : vec3<f32>,
    rayDistance : f32) {
    hits[rayIndex] = SparseVoxelSceneRayHit(
        vec4<f32>(position, rayDistance),
        vec3<i32>(-2147483648),
        SPARSE_VOXEL_SCENE_RAY_STATUS_HIT,
        vec4<u32>(ray.seedFlags.x, ray.pixelRayLight.w,
                  SPARSE_VOXEL_SCENE_RAY_BACKEND_SCREEN, ray.seedFlags.z));
}

// Adjacent voxel faces on one flat sheet have different voxel identities even
// though they are the same receiver plane. Treating each cell boundary as a
// new depth-buffer occluder turns sub-pixel depth reconstruction differences
// into acne, particularly at grazing angles. Only collapse faces from the same
// instance whose geometric normals and planes agree; a real parallel blocker
// remains distinct once it is more than one receiver pixel away.
fn screen_trace_same_receiver_sheet(
    receiver : ScreenTraceSurface, candidate : ScreenTraceSurface,
    receiverIdentity : vec2<u32>, candidateIdentity : vec2<u32>) -> bool {
    // Terrain page ownership is not a geometric boundary. Adjacent coplanar
    // faces frequently belong to different streamed instances, so requiring
    // the instance id to match turns a page seam into an occluder. Identity is
    // still used by the exact receiver-voxel test at the call site; the sheet
    // test is intentionally geometric.
    if (dot(receiver.normal, candidate.normal) < 0.99) {
        return false;
    }
    let planeSeparation = abs(dot(
        candidate.relativePosition - receiver.relativePosition,
        receiver.normal));
    let receiverPixel = screen_trace_pixel_size(receiver.viewDepth);
    let candidatePixel = screen_trace_pixel_size(candidate.viewDepth);
    // Both positions are camera-relative, so this remains sub-millimetre near
    // the receiver instead of changing by centimetres as the camera rotates
    // at planet-scale coordinates.
    let relativeMagnitude = max(
        max(max(abs(receiver.relativePosition.x),
                abs(receiver.relativePosition.y)),
            abs(receiver.relativePosition.z)),
        max(max(abs(candidate.relativePosition.x),
                abs(candidate.relativePosition.y)),
            abs(candidate.relativePosition.z)));
    let numericTolerance = max(relativeMagnitude * 4.7683716e-7, 1.0e-5);
    let planeTolerance = max(
        min(receiverPixel, candidatePixel) * 2.0,
        max(numericTolerance, 1.0e-4));
    return planeSeparation <= planeTolerance;
}

fn screen_trace_leaf(
    ray : SparseVoxelSceneRay, rayIndex : u32,
    receiver : ScreenTraceSurface, direction : vec3<f32>,
    receiverIdentity : vec2<u32>, pixel : vec2<i32>,
    intervalDistance : vec2<f32>,
    traceDistance : vec2<f32>, originOffset : f32,
    cameraForward : vec3<f32>) -> bool {
    let surface = screen_trace_surface(pixel);
    if (!surface.valid) { return false; }
    // A screen ray begins on the reconstructed receiver, so another pixel
    // covered by that exact same voxel is not an occluder. Without this test,
    // the receiver's depth plane/slab can intersect the ray again a pixel
    // away and produce the sparse black terrain cracks seen at close range.
    // Identity rejection is exact and does not move the ray origin.
    let candidateIdentity = vec2<u32>(
        textureLoad(instanceIdTexture, pixel, 0).x,
        textureLoad(voxelIdTexture, pixel, 0).x);
    if (all(candidateIdentity == receiverIdentity)) {
        screen_trace_stat_add(SCREEN_TRACE_STAT_RECEIVER_ID_REJECTS, 1u);
        return false;
    }
    if (screen_trace_same_receiver_sheet(
            receiver, surface, receiverIdentity, candidateIdentity)) {
        screen_trace_stat_add(SCREEN_TRACE_STAT_RECEIVER_SHEET_REJECTS, 1u);
        return false;
    }

    let denominator = dot(direction, surface.normal);
    if (abs(denominator) > 1.0e-6) {
        let planeDistance =
            dot(surface.relativePosition - receiver.relativePosition,
                surface.normal) /
            denominator;
        if (planeDistance >= traceDistance.x &&
            planeDistance <= traceDistance.y) {
            let position = receiver.position + direction * planeDistance;
            let projected = screen_trace_project(position);
            if (projected.valid) {
                let dilation = select(
                    0.05, 0.75,
                    screen_trace_discontinuous(pixel, surface.viewDepth));
                let lower = vec2<f32>(pixel) - vec2<f32>(dilation);
                let upper = vec2<f32>(pixel + vec2<i32>(1)) +
                    vec2<f32>(dilation);
                if (all(projected.pixel >= lower) &&
                    all(projected.pixel <= upper)) {
                    screen_trace_stat_add(SCREEN_TRACE_STAT_PLANE_HITS, 1u);
                    screen_trace_record_hit(
                        ray, rayIndex, position,
                        max(planeDistance - originOffset, 0.0));
                    return true;
                }
            }
        }
    }

    // A single depth layer cannot reconstruct a side face, including the
    // exactly plane-parallel case. Any finite extrusion here is invented
    // geometry and showed up as an angle-dependent line eventually. Hidden
    // side faces are always handled by the overlapping Tree64 trace.
    return false;
}

@compute @workgroup_size(64, 1, 1)
fn traceSparseVoxelSceneRays(@builtin(global_invocation_id) gid : vec3<u32>) {
    let workIndex = gid.x;
    if (arrayLength(&activeRayState) < 4u ||
        workIndex >= activeRayState[3] ||
        workIndex >= arrayLength(&activeRayIndices)) { return; }
    let rayIndex = activeRayIndices[workIndex];
    if (rayIndex >= settings.viewport.z || rayIndex >= arrayLength(&rays) ||
        rayIndex >= arrayLength(&hits)) { return; }
    let ray = rays[rayIndex];
    if ((ray.seedFlags.y & SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) == 0u) {
        hits[rayIndex] = sparse_voxel_invalid_scene_hit(
            ray, SPARSE_VOXEL_SCENE_RAY_STATUS_INVALID,
            SPARSE_VOXEL_SCENE_RAY_BACKEND_NONE);
        return;
    }
    if (settings.traceParams.z == 0.0) {
        hits[rayIndex] = sparse_voxel_invalid_scene_hit(
            ray, SPARSE_VOXEL_SCENE_RAY_STATUS_PENDING,
            SPARSE_VOXEL_SCENE_RAY_BACKEND_NONE);
        return;
    }
    screen_trace_stat_add(SCREEN_TRACE_STAT_ACTIVE_RAYS, 1u);

    let rawDirection = ray.directionMaximumDistance.xyz;
    if (dot(rawDirection, rawDirection) < 1.0e-12) {
        hits[rayIndex] = sparse_voxel_invalid_scene_hit(
            ray, SPARSE_VOXEL_SCENE_RAY_STATUS_MISS_TMAX,
            SPARSE_VOXEL_SCENE_RAY_BACKEND_SCREEN);
        screen_trace_stat_terminal(
            SPARSE_VOXEL_SCENE_RAY_STATUS_MISS_TMAX, 0u, 0u, true);
        return;
    }
    let direction = normalize(rawDirection);
    let sourcePixel = vec2<i32>(
        min(ray.pixelRayLight.xy, screen_trace_dimensions() - vec2<u32>(1u)));
    let receiver = screen_trace_surface(sourcePixel);
    if (!receiver.valid) {
        hits[rayIndex] = sparse_voxel_invalid_scene_hit(
            ray, SPARSE_VOXEL_SCENE_RAY_STATUS_PENDING,
            SPARSE_VOXEL_SCENE_RAY_BACKEND_NONE);
        return;
    }
    let receiverIdentity = vec2<u32>(
        textureLoad(instanceIdTexture, sourcePixel, 0).x,
        textureLoad(voxelIdTexture, sourcePixel, 0).x);

    let originOffset = dot(
        ray.originMinimumDistance.xyz - receiver.position, direction);
    let startDistance = max(
        screen_trace_pixel_size(receiver.viewDepth) *
            max(settings.traceParams.w, 1.0),
        1.0e-5);
    var endDistance = min(
        max(ray.directionMaximumDistance.w, 0.0),
        max(settings.traceParams.y, 1.0e-4));
    if (endDistance <= startDistance) {
        hits[rayIndex] = sparse_voxel_unresolved_scene_hit_at(
            ray, SPARSE_VOXEL_SCENE_RAY_STATUS_MISS_TMAX,
            SPARSE_VOXEL_SCENE_RAY_BACKEND_SCREEN,
            max(endDistance - originOffset, 0.0));
        screen_trace_stat_terminal(
            SPARSE_VOXEL_SCENE_RAY_STATUS_MISS_TMAX, 0u, 0u, true);
        return;
    }

    let cameraForward = normalize(
        (raster.cameraToWorld * vec4<f32>(0.0, 0.0, 1.0, 0.0)).xyz);
    let axialDirection = dot(direction, cameraForward);
    // Clip before the camera plane so both projected endpoints have positive w.
    if (receiver.viewDepth + axialDirection * endDistance <= 1.0e-4) {
        if (axialDirection >= -1.0e-8) {
            hits[rayIndex] = sparse_voxel_invalid_scene_hit(
                ray, SPARSE_VOXEL_SCENE_RAY_STATUS_PENDING,
                SPARSE_VOXEL_SCENE_RAY_BACKEND_NONE);
            return;
        }
        endDistance = min(
            endDistance,
            max(startDistance,
                (receiver.viewDepth - 1.0e-4) / -axialDirection));
    }

    let startProjection = screen_trace_project(
        receiver.position + direction * startDistance);
    let endProjection = screen_trace_project(
        receiver.position + direction * endDistance);
    if (!startProjection.valid || !endProjection.valid) {
        hits[rayIndex] = sparse_voxel_unresolved_scene_hit_at(
            ray, SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_SCENE,
            SPARSE_VOXEL_SCENE_RAY_BACKEND_SCREEN, 0.0);
        screen_trace_stat_terminal(
            SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_SCENE, 0u, 0u, false);
        return;
    }
    let screenDelta = endProjection.pixel - startProjection.pixel;
    if (dot(screenDelta, screenDelta) < 1.0) {
        hits[rayIndex] = sparse_voxel_unresolved_scene_hit_at(
            ray, SPARSE_VOXEL_SCENE_RAY_STATUS_MISS_TMAX,
            SPARSE_VOXEL_SCENE_RAY_BACKEND_SCREEN,
            max(endDistance - originOffset, 0.0));
        screen_trace_stat_terminal(
            SPARSE_VOXEL_SCENE_RAY_STATUS_MISS_TMAX, 0u, 0u, true);
        return;
    }

    let clipped = screen_trace_clip_segment(
        startProjection.pixel, screenDelta,
        vec2<f32>(screen_trace_dimensions()));
    if (clipped.x > clipped.y) {
        hits[rayIndex] = sparse_voxel_unresolved_scene_hit_at(
            ray, SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_SCENE,
            SPARSE_VOXEL_SCENE_RAY_BACKEND_SCREEN, 0.0);
        screen_trace_stat_terminal(
            SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_SCENE, 0u, 0u, false);
        return;
    }

    let mipCount = clamp(settings.controls.x, 1u, SCREEN_TRACE_MAX_MIPS);
    let maximumMip = mipCount - 1u;
    // Start no coarser than the level whose cell already contains the whole
    // segment. A near-field ray covers a few dozen pixels, so the levels above
    // that describe most of the screen: they can never reject anything the ray
    // was going to touch, and each one still costs a visit and a descent.
    let segmentPixels =
        max(abs(screenDelta.x), abs(screenDelta.y)) *
        max(clipped.y - clipped.x, 0.0);
    let segmentLevel = u32(max(ceil(log2(max(segmentPixels, 1.0))), 0.0));
    let coarsestLevel = min(maximumMip, segmentLevel);
    let dynamicLeafLimit = min(
        max(settings.viewport.w, 1u),
        max(8u, u32(ceil(segmentPixels * 1.25)) + 8u));
    var level = coarsestLevel;
    var screenFraction = clipped.x;
    var reachedDistance = startDistance;
    var leafVisits = 0u;
    var nodeVisits = 0u;
    let screenEpsilon = 1.0e-4 /
        max(max(abs(screenDelta.x), abs(screenDelta.y)), 1.0);
    let visitLimit = min(
        SCREEN_TRACE_MAX_VISITS,
        max(dynamicLeafLimit * 8u, mipCount * 4u + 8u));

    for (var visit = 0u; visit < visitLimit; visit++) {
        if (screenFraction > clipped.y ||
            leafVisits >= dynamicLeafLimit) { break; }
        nodeVisits += 1u;
        let screenPosition = clamp(
            startProjection.pixel + screenDelta * screenFraction,
            vec2<f32>(0.0),
            vec2<f32>(screen_trace_dimensions()) - vec2<f32>(1.0e-3));
        let cellSize = f32(1u << level);
        let node = min(
            vec2<u32>(floor(screenPosition / cellSize)),
            screen_trace_mip_dimensions(level) - vec2<u32>(1u));
        let exitFraction = screen_trace_node_exit(
            startProjection.pixel, screenDelta, node, level,
            screenFraction, clipped.y);
        let worldFraction0 = screen_trace_world_fraction(
            screenFraction, startProjection.clipW, endProjection.clipW);
        let worldFraction1 = screen_trace_world_fraction(
            exitFraction, startProjection.clipW, endProjection.clipW);
        let distance0 = mix(startDistance, endDistance, worldFraction0);
        let distance1 = mix(startDistance, endDistance, worldFraction1);
        reachedDistance = max(reachedDistance, max(distance0, distance1));

        let bounds = depthBounds[screen_trace_bounds_index(node, level)];
        var overlaps = bounds.x >= 0.0;
        if (overlaps) {
            let surfaceNear = screen_trace_view_depth(
                screen_trace_metric_device_depth(bounds.x));
            let surfaceFar = screen_trace_view_depth(
                screen_trace_metric_device_depth(bounds.y));
            let rayDepth0 = receiver.viewDepth + axialDirection * distance0;
            let rayDepth1 = receiver.viewDepth + axialDirection * distance1;
            let rayNear = min(rayDepth0, rayDepth1);
            let rayFar = max(rayDepth0, rayDepth1);
            let bias = screen_trace_pixel_size(surfaceNear) * 1.5;
            // The same slab the leaf test will apply, so the hierarchy never
            // descends into a node whose leaves cannot produce a hit.
            let thickness = screen_trace_thickness(
                abs(distance1 - distance0), bias);
            overlaps = rayFar > surfaceNear + bias * 0.5 &&
                       rayNear < surfaceFar + thickness;
        }

        if (overlaps && level > 0u) {
            level -= 1u;
            continue;
        }
        if (overlaps) {
            leafVisits += 1u;
            if (screen_trace_leaf(
                    ray, rayIndex, receiver, direction, receiverIdentity,
                    vec2<i32>(node),
                    vec2<f32>(min(distance0, distance1),
                              max(distance0, distance1)),
                    vec2<f32>(startDistance, endDistance), originOffset,
                    cameraForward)) {
                screen_trace_stat_terminal(
                    SPARSE_VOXEL_SCENE_RAY_STATUS_HIT,
                    leafVisits, nodeVisits, false);
                return;
            }
        }

        if (exitFraction >= clipped.y - screenEpsilon) {
            screenFraction = clipped.y + screenEpsilon;
            break;
        }
        screenFraction = exitFraction + screenEpsilon;
        level = min(level + 1u, coarsestLevel);
    }

    let leftScreen = clipped.y < 1.0 - screenEpsilon;
    hits[rayIndex] = sparse_voxel_unresolved_scene_hit_at(
        ray,
        select(SPARSE_VOXEL_SCENE_RAY_STATUS_MISS_TMAX,
               SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_SCENE, leftScreen),
        SPARSE_VOXEL_SCENE_RAY_BACKEND_SCREEN,
        max(reachedDistance - originOffset, 0.0));
    screen_trace_stat_terminal(
        select(SPARSE_VOXEL_SCENE_RAY_STATUS_MISS_TMAX,
               SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_SCENE, leftScreen),
        leafVisits, nodeVisits, false);
}
