// Low-resolution screen-space sky importance guide.
//
// Each 16x16 receiver tile owns a 4x4 histogram in the two uniform parameters
// of cosine-hemisphere sampling (squared radius and azimuth). The light-field
// pass records resolved sky visibility into feedback. This pass converts the
// feedback into a temporally filtered CDF for the next frame's ray creation.

#include "globalIllumination/worldgi/wgsl/worldgi-types.wgsl"
#include "sparse_voxel_sky.wgsl"
#include "sparse_voxel_sky_guide_common.wgsl"

struct SparseVoxelSkyGuideSettings {
    // xy: raster dimensions, z: tile count, w: enabled.
    dimensions : vec4<u32>,
    // x: learning rate, y: uniform prior, z: minimum observations for a full
    // learning-rate update, w: unused.
    controls : vec4<f32>,
}

@group(0) @binding(0) var<uniform> settings : SparseVoxelSkyGuideSettings;
// Per bin: observation count, then quantized visible sky luminance sum.
@group(0) @binding(1) var<storage, read_write> feedback : array<atomic<u32>>;
// One monotonically increasing 16-entry CDF per tile.
@group(0) @binding(2) var<storage, read_write> guideCDF : array<f32>;

@compute @workgroup_size(64, 1, 1)
fn resolveSparseVoxelSkyGuide(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let tileIndex = globalId.x;
    let tileCount = settings.dimensions.z;
    if (tileIndex >= tileCount) { return; }
    let cdfBase = tileIndex * SPARSE_VOXEL_SKY_GUIDE_BIN_COUNT;
    let feedbackBase = cdfBase * SPARSE_VOXEL_SKY_GUIDE_FEEDBACK_STRIDE;
    if (cdfBase + SPARSE_VOXEL_SKY_GUIDE_BIN_COUNT > arrayLength(&guideCDF) ||
        feedbackBase + SPARSE_VOXEL_SKY_GUIDE_BIN_COUNT *
            SPARSE_VOXEL_SKY_GUIDE_FEEDBACK_STRIDE > arrayLength(&feedback)) {
        return;
    }

    var oldProbabilities : array<f32, 16>;
    var learnedScores : array<f32, 16>;
    var oldTotal = 0.0;
    var learnedTotal = 0.0;
    var totalObservations = 0u;
    var resetGuide = false;
    var previousCDF = 0.0;
    let prior = max(settings.controls.y, 1.0e-5);
    for (var bin = 0u; bin < SPARSE_VOXEL_SKY_GUIDE_BIN_COUNT;
         bin = bin + 1u) {
        let currentCDF = clamp(guideCDF[cdfBase + bin], 0.0, 1.0);
        let oldProbability = max(currentCDF - previousCDF, 0.0);
        oldProbabilities[bin] = oldProbability;
        oldTotal = oldTotal + oldProbability;
        previousCDF = currentCDF;

        let feedbackIndex = feedbackBase +
            bin * SPARSE_VOXEL_SKY_GUIDE_FEEDBACK_STRIDE;
        var observations = atomicExchange(&feedback[feedbackIndex], 0u);
        if (bin == 0u) {
            resetGuide = (observations &
                SPARSE_VOXEL_SKY_GUIDE_FEEDBACK_RESET_BIT) != 0u;
            observations = observations &
                ~SPARSE_VOXEL_SKY_GUIDE_FEEDBACK_RESET_BIT;
        }
        let radiance = atomicExchange(&feedback[feedbackIndex + 1u], 0u);
        totalObservations = totalObservations + observations;
        let meanContribution = select(
            0.0,
            f32(radiance) /
                (f32(observations) * SPARSE_VOXEL_SKY_GUIDE_RADIANCE_SCALE),
            observations != 0u);
        let score = max(meanContribution, 0.0) + prior;
        learnedScores[bin] = score;
        learnedTotal = learnedTotal + score;
    }

    let uniformProbability = 1.0 /
        f32(SPARSE_VOXEL_SKY_GUIDE_BIN_COUNT);
    let oldValid = !resetGuide && oldTotal > 0.99 && oldTotal < 1.01;
    let observationTarget = max(settings.controls.z, 1.0);
    let confidence = clamp(
        f32(totalObservations) / observationTarget, 0.0, 1.0);
    let learningRate = clamp(settings.controls.x, 0.0, 1.0) * confidence;
    var cumulative = 0.0;
    for (var bin = 0u; bin < SPARSE_VOXEL_SKY_GUIDE_BIN_COUNT;
         bin = bin + 1u) {
        let oldProbability = select(
            uniformProbability,
            oldProbabilities[bin] / max(oldTotal, 1.0e-8),
            oldValid);
        let learnedProbability = learnedScores[bin] /
            max(learnedTotal, 1.0e-8);
        cumulative = cumulative + mix(
            oldProbability, learnedProbability, learningRate);
        guideCDF[cdfBase + bin] = select(
            min(cumulative, 1.0), 1.0,
            bin + 1u == SPARSE_VOXEL_SKY_GUIDE_BIN_COUNT);
    }
}
