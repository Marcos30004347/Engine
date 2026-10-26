// Folds this frame's traced probe rays into the world irradiance cache.
//
// One invocation per refreshed probe. Each reads its own rays' results, builds
// a spherical-harmonic radiance estimate, and blends it into the stored
// coefficients with hysteresis. The blend is the entire point: a probe cannot
// swing to a new value in one frame no matter what its rays did, which is
// exactly the failure mode screen probes have.
#include "worldgi_ddgi_common.wgsl"

struct WorldGIDDGITree64Ray {
    originMaximumDistance : vec4<f32>,
    direction : vec4<f32>,
}

struct WorldGIDDGITree64Result {
    globalCell : vec3<i32>,
    status : u32,
    distance : f32,
    skippedDistance : f32,
    queryCount : u32,
    baseCellSteps : u32,
    highestLevel : u32,
    contentGeneration : u32,
    reserved0 : u32,
    reserved1 : u32,
}

const WORLDGI_DDGI_TREE64_MISS : u32 = 1u;
const WORLDGI_DDGI_TREE64_HIT : u32 = 2u;

struct WorldGIDDGIProbeSH {
    coefficients : array<vec4<f32>, WORLDGI_DDGI_SH_COEFFICIENTS>,
}

struct WorldGIDDGIProbeState {
    relocationConfidence : vec4<f32>,
    generationClassificationSamplesFlags : vec4<u32>,
}

const WORLDGI_DDGI_CLASSIFICATION_ACTIVE : u32 = 2u;
const WORLDGI_DDGI_FLAG_VALID_HISTORY : u32 = 1u;

@group(0) @binding(0) var<uniform> settings : WorldGIDDGISettings;
@group(0) @binding(1) var<storage, read> volumes : array<WorldGIDDGIVolume>;
@group(0) @binding(2) var<storage, read> results : array<WorldGIDDGITree64Result>;
@group(0) @binding(3) var<storage, read_write> probeSH : array<WorldGIDDGIProbeSH>;
@group(0) @binding(4) var<storage, read_write> probeState : array<WorldGIDDGIProbeState>;
// The refresh cursor lives on the GPU and advances itself. It used to ride in
// the settings uniform, where the kernel only ever saw the frame-zero value no
// matter how the upload was arranged - the CPU cursor stepped 0, 864, 1728
// while `kernelCursor` read back 0 forever, so one batch of 864 probes was
// refreshed repeatedly and the other 6,048 were never visited at all.
@group(0) @binding(5) var<storage, read_write> schedule : array<u32>;
// Parallel to `results`: each ray carries the probe slot it was generated for.
@group(0) @binding(6) var<storage, read> rays : array<WorldGIDDGITree64Ray>;

@compute @workgroup_size(64, 1, 1)
fn updateWorldGIDDGIProbes(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let probesThisFrame = settings.cascadeRaysProbes.z;
    let batchProbe = globalId.x;
    if (batchProbe >= probesThisFrame) {
        return;
    }
    let rayCount = max(settings.cascadeRaysProbes.y, 1u);
    let totalProbes = max(settings.cascadeRaysProbes.w, 1u);
    // Taken from the ray itself rather than from a shared cursor.
    let base = batchProbe * rayCount;
    if (base >= arrayLength(&rays)) {
        return;
    }
    let cursor = select(0u, schedule[0], arrayLength(&schedule) > 0u);
    let probeSlot = u32(max(rays[base].direction.w, 0.0)) % totalProbes;
    if (probeSlot >= arrayLength(&probeSH) ||
        probeSlot >= arrayLength(&probeState)) {
        return;
    }

    // The same rotation the ray generator used. Directions are recomputed
    // rather than read back, which keeps the ray buffer write-only from this
    // kernel's point of view and avoids a second 32-byte fetch per ray.
    // Seeded from the GPU-owned cursor and refresh generation rather than the
    // uniform's frame index, for the same reason: those fields never change.
    let rotationSeed = worldGIDDGIHash(
        cursor * 9781u + select(0u, schedule[1], arrayLength(&schedule) > 1u));
    let rotation = vec2<f32>(
        f32(rotationSeed & 0xffffu) / 65535.0 * 6.2831853,
        f32((rotationSeed >> 16u) & 0xffffu) / 65535.0 * 6.2831853);

    var accumulated : array<vec3<f32>, 9>;
    for (var coefficient = 0u; coefficient < 9u; coefficient = coefficient + 1u) {
        accumulated[coefficient] = vec3<f32>(0.0);
    }
    var validRays = 0u;
    var sunVisible = false;
    var closestHit = settings.blendTuning.y;
    for (var ray = 0u; ray < rayCount; ray = ray + 1u) {
        let resultIndex = base + ray;
        if (resultIndex >= arrayLength(&results)) {
            break;
        }
        let result = results[resultIndex];
        // Anything that is not a definite miss or hit is *unknown*, and unknown
        // must not be folded in as darkness. A tracer that cannot answer -
        // unpublished data, a query limit - previously fabricated occlusion
        // elsewhere in this renderer and produced scene-wide darkening; the
        // same mistake here would bake that darkness into a cache that then
        // takes hundreds of frames to forget it.
        if (result.status != WORLDGI_DDGI_TREE64_MISS &&
            result.status != WORLDGI_DDGI_TREE64_HIT) {
            continue;
        }
        // Ray zero carries sun visibility, not a sky sample.
        if (ray == 0u) {
            sunVisible = result.status == WORLDGI_DDGI_TREE64_MISS;
            if (result.status == WORLDGI_DDGI_TREE64_HIT) {
                closestHit = min(closestHit, max(result.distance, 0.0));
            }
            continue;
        }
        let direction = worldGIDDGIRayDirection(ray, rayCount, rotation);
        var radiance = vec3<f32>(0.0);
        if (result.status == WORLDGI_DDGI_TREE64_MISS) {
            radiance = worldGIDDGISkyRadiance(settings, direction);
        } else {
            closestHit = min(closestHit, max(result.distance, 0.0));
        }
        let basis = worldGIDDGIBasis(direction);
        for (var coefficient = 0u; coefficient < 9u; coefficient = coefficient + 1u) {
            accumulated[coefficient] += radiance * basis[coefficient];
        }
        validRays = validRays + 1u;
    }
    if (validRays == 0u) {
        return;
    }

    // Monte Carlo estimate over the sphere: the directions are uniform, so the
    // estimator is the mean times the full solid angle.
    let normalization = 12.566370614 / f32(validRays);
    // The sun is a delta lobe: its spherical-harmonic projection is its
    // irradiance times the basis evaluated in its direction, with no solid
    // angle factor. Adding it here rather than sampling it is what makes an
    // outdoor cache carry the right amount of light.
    // Direct sun in the cache is a *4 m resolution shadow*. Each probe answers
    // "is the sun visible" with one ray, so on a shadowed surface adjacent
    // probes disagree and the trilinear blend renders that disagreement as
    // blotchy, dithered shading where the renderer's own per-pixel shadow would
    // be smooth. Off by default for that reason: the cache is meant to carry
    // the indirect term.
    let sunEnabled = settings.frameSeedCursor.w != 0u;
    let sunDirection = normalize(settings.sunDirectionIntensity.xyz);
    let sunBasis = worldGIDDGIBasis(sunDirection);
    let sunIrradiance = settings.sunColorCosine.xyz *
        settings.sunDirectionIntensity.w;
    var state = probeState[probeSlot];
    let hadHistory =
        (state.generationClassificationSamplesFlags.w &
         WORLDGI_DDGI_FLAG_VALID_HISTORY) != 0u &&
        state.generationClassificationSamplesFlags.x ==
            settings.frameSeedCursor.w;
    // A probe with no history takes the estimate outright. Blending from zero
    // would darken it for the many frames the hysteresis takes to climb, and a
    // newly scrolled-in slab of probes would read as a moving shadow.
    let blend = select(1.0, 1.0 - clamp(settings.blendTuning.x, 0.0, 0.999),
        hadHistory);

    // Nine RGB coefficients, one per vec4.
    for (var coefficient = 0u; coefficient < WORLDGI_DDGI_SH_COEFFICIENTS;
         coefficient = coefficient + 1u) {
        var estimate = accumulated[coefficient] * normalization;
        if (sunVisible && sunEnabled) {
            estimate += sunIrradiance * sunBasis[coefficient];
        }
        let previous = probeSH[probeSlot].coefficients[coefficient].xyz;
        probeSH[probeSlot].coefficients[coefficient] =
            vec4<f32>(mix(previous, estimate, blend), 0.0);
    }

    // Temporary instrument: publish what this invocation actually read, so the
    // CPU can compare it against what it believes it uploaded.
    state.relocationConfidence = vec4<f32>(
        f32(cursor), f32(totalProbes), f32(probesThisFrame), closestHit);
    // Publish the schedule parameters for the advance kernel. Reading the
    // settings uniform from that second pipeline returned zeros - the cursor
    // advanced by (0 + 0) % 1 and stayed put forever while its generation
    // counter incremented normally, which is how the difference was isolated.
    // The values are identical for every invocation, so the races are benign.
    schedule[2] = probesThisFrame;
    schedule[3] = totalProbes;
    state.generationClassificationSamplesFlags = vec4<u32>(
        settings.frameSeedCursor.w,
        WORLDGI_DDGI_CLASSIFICATION_ACTIVE,
        min(state.generationClassificationSamplesFlags.z + validRays, 0xffffffu),
        WORLDGI_DDGI_FLAG_VALID_HISTORY);
    probeState[probeSlot] = state;
}

// One invocation, dispatched after the refresh above so every reader of the
// cursor has finished with it. Advancing inside the update dispatch would race:
// an invocation that read the new value would fold its rays into a different
// probe than the one they were traced for.
@compute @workgroup_size(1, 1, 1)
fn advanceWorldGIDDGICursor(@builtin(global_invocation_id) id : vec3<u32>) {
    if (id.x != 0u || arrayLength(&schedule) < 2u) {
        return;
    }
    if (arrayLength(&schedule) < 4u) {
        return;
    }
    let totalProbes = max(schedule[3], 1u);
    let probesThisFrame = schedule[2];
    schedule[0] = (schedule[0] + probesThisFrame) % totalProbes;
    schedule[1] = schedule[1] + 1u;
}
