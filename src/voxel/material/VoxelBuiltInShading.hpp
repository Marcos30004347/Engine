#pragma once

#include "voxel/material/VoxelShadingIR.hpp"
#include "voxel/svraster/SparseVoxelMicroMaterial.hpp"

#include <cstdint>

namespace voxel::material
{

// Compatibility compiler for the five pre-IR profiles. New callers can use
// the returned program directly; legacy descriptors remain available for old
// pages and as a fallback during migration.
VoxelShadingProgram builtInVoxelShadingProgram(
    const svraster::SparseVoxelMicroShadingDescriptor &shading);

VoxelShadingProgram noisyLinearVoxelShadingProgram(
    uint32_t variationStrength, float hueVariation = 0.0f,
    float brightnessVariation = 0.0f);

VoxelShadingProgram grassOverDirtVoxelShadingProgram(
    float depthStart, float depthEnd, float slopeStartDegrees,
    float slopeEndDegrees, float topRoughness, float soilRoughness,
    float hueVariation = 0.0f, float brightnessVariation = 0.0f);

// The default terrain graph uses the region variation field for flat-top hue
// and stable micro identity for deterministic brightness. Bound grass/dirt
// fields remain available to custom graphs; depth, side/riser exposure, and
// terrain slope are the default graph's only soil selectors.
VoxelShadingProgram grassOverDirtFieldVoxelShadingProgram(
    float depthStart, float depthEnd,
    float slopeStartDegrees, float slopeEndDegrees,
    float topRoughness, float soilRoughness,
    float hueVariation = 0.0f, float brightnessVariation = 0.0f);

} // namespace voxel::material
