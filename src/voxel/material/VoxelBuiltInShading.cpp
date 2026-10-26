#include "voxel/material/VoxelBuiltInShading.hpp"

#include <algorithm>

namespace voxel::material
{
namespace
{

void emit(
    VoxelShadingProgram &program, const VoxelShadingOpcode opcode,
    const uint32_t destination, const uint32_t source0 = 0u,
    const uint32_t source1 = 0u, const float immediate0 = 0.0f,
    const float immediate1 = 0.0f)
{
  program.instructions.push_back(
      {opcode, destination, source0, source1, immediate0, immediate1});
}

} // namespace

VoxelShadingProgram noisyLinearVoxelShadingProgram(
    const uint32_t variationStrength, const float hueVariation,
    const float brightnessVariation)
{
  using Opcode = VoxelShadingOpcode;
  VoxelShadingProgram program{};
  const float variation =
      static_cast<float>(variationStrength & 0xffu) / 255.0f;
  emit(program, Opcode::LoadNormalizedDepth, 0u);
  emit(program, Opcode::LoadStableRandom, 1u);
  emit(program, Opcode::ScaleBias, 1u, 1u, 0u, variation * 2.0f, -variation);
  emit(program, Opcode::Add, 0u, 0u, 1u);
  emit(program, Opcode::Clamp01, 0u, 0u);
  if (brightnessVariation > 0.0f)
  {
    const float amplitude = std::clamp(brightnessVariation, 0.0f, 0.5f);
    emit(program, Opcode::LoadStableRandom, 3u);
    emit(program, Opcode::ScaleBias, 3u, 3u, 0u, amplitude * 2.0f,
         1.0f - amplitude);
  }
  if (hueVariation != 0.0f)
    emit(program, Opcode::LoadConstant, 4u, 0u, 0u,
         std::clamp(hueVariation, -1.0f, 1.0f));
  emit(program, Opcode::OutputLayerWeight, 0u, 0u);
  if (hueVariation != 0.0f)
    emit(program, Opcode::OutputHueVariation, 0u, 4u);
  if (brightnessVariation > 0.0f)
    emit(program, Opcode::OutputBrightness, 0u, 3u);
  return program;
}

VoxelShadingProgram builtInVoxelShadingProgram(
    const svraster::SparseVoxelMicroShadingDescriptor &shading)
{
  using Opcode = VoxelShadingOpcode;
  VoxelShadingProgram program{};
  const float variation =
      static_cast<float>(shading.variationStrength & 0xffu) / 255.0f;
  switch (shading.profile)
  {
  case svraster::SparseVoxelMicroShadingProfile::Linear:
    emit(program, Opcode::LoadNormalizedDepth, 0u);
    break;
  case svraster::SparseVoxelMicroShadingProfile::HardCrust:
  {
    const float authored =
        static_cast<float>(shading.parameter0 & 0xffu) / 255.0f;
    emit(program, Opcode::LoadNormalizedDepth, 0u);
    emit(program, Opcode::GreaterThan, 0u, 0u, 0u,
         authored > 0.0f ? authored : 0.25f);
    break;
  }
  case svraster::SparseVoxelMicroShadingProfile::NoisyLinear:
    return noisyLinearVoxelShadingProgram(shading.variationStrength);
  case svraster::SparseVoxelMicroShadingProfile::ExposedSides:
    // This is algebraically equivalent to the exact heightfield path: sides
    // select soil, classified slopes blend by depth, and flat exposed tops add
    // a deterministic amount of soil without changing micro identity.
    emit(program, Opcode::LoadSideOrRiser, 0u);
    emit(program, Opcode::LoadNormalizedDepth, 1u);
    emit(program, Opcode::LoadStableRandom, 2u);
    emit(program, Opcode::ScaleBias, 2u, 2u, 0u,
         variation * 0.85f, variation * 0.15f);
    emit(program, Opcode::OneMinus, 3u, 1u);
    emit(program, Opcode::Multiply, 2u, 2u, 3u);
    emit(program, Opcode::LoadTerrainSlopeClassified, 4u);
    emit(program, Opcode::OneMinus, 4u, 4u);
    emit(program, Opcode::Multiply, 2u, 2u, 4u);
    emit(program, Opcode::Add, 1u, 1u, 2u);
    emit(program, Opcode::Maximum, 0u, 0u, 1u);
    break;
  case svraster::SparseVoxelMicroShadingProfile::FoliageTint:
    emit(program, Opcode::LoadNormalizedDepth, 0u);
    emit(program, Opcode::ScaleBias, 0u, 0u, 0u, 0.65f, 0.0f);
    emit(program, Opcode::LoadStableRandom, 1u);
    emit(program, Opcode::ScaleBias, 1u, 1u, 0u,
         variation * 0.35f, 0.0f);
    emit(program, Opcode::Add, 0u, 0u, 1u);
    emit(program, Opcode::Clamp01, 0u, 0u);
    break;
  }
  emit(program, Opcode::OutputLayerWeight, 0u, 0u);
  return program;
}

VoxelShadingProgram grassOverDirtVoxelShadingProgram(
    const float depthStart, const float depthEnd,
    const float slopeStartDegrees, const float slopeEndDegrees,
    const float topRoughness, const float soilRoughness,
    const float hueVariation, const float brightnessVariation)
{
  using Opcode = VoxelShadingOpcode;
  VoxelShadingProgram program{};
  const float start = std::clamp(depthStart, 0.0f, 0.99f);
  const float end = std::clamp(depthEnd, start + 0.01f, 1.0f);
  const float slopeStart =
      std::clamp(slopeStartDegrees / 90.0f, 0.0f, 0.99f);
  const float slopeEnd =
      std::clamp(slopeEndDegrees / 90.0f, slopeStart + 0.01f, 1.0f);
  emit(program, Opcode::LoadSideOrRiser, 0u);
  emit(program, Opcode::LoadNormalizedDepth, 1u);
  emit(program, Opcode::Smoothstep, 1u, 1u, 0u, start, end);
  emit(program, Opcode::LoadTerrainSlope, 2u);
  emit(program, Opcode::Smoothstep, 2u, 2u, 0u, slopeStart, slopeEnd);
  emit(program, Opcode::Maximum, 0u, 0u, 1u);
  emit(program, Opcode::Maximum, 0u, 0u, 2u);
  if (brightnessVariation > 0.0f)
  {
    const float amplitude = std::clamp(brightnessVariation, 0.0f, 0.5f);
    emit(program, Opcode::LoadStableRandom, 3u);
    emit(program, Opcode::ScaleBias, 3u, 3u, 0u, amplitude * 2.0f,
         1.0f - amplitude);
  }
  if (hueVariation != 0.0f)
    emit(program, Opcode::LoadConstant, 4u, 0u, 0u,
         std::clamp(hueVariation, -1.0f, 1.0f));
  const float top = std::clamp(topRoughness, 0.0f, 1.0f);
  const float soil = std::clamp(soilRoughness, 0.0f, 1.0f);
  emit(program, Opcode::ScaleBias, 5u, 0u, 0u, soil - top, top);
  emit(program, Opcode::OutputLayerWeight, 0u, 0u);
  if (hueVariation != 0.0f)
    emit(program, Opcode::OutputHueVariation, 0u, 4u);
  if (brightnessVariation > 0.0f)
    emit(program, Opcode::OutputBrightness, 0u, 3u);
  emit(program, Opcode::OutputRoughness, 0u, 5u);
  return program;
}

VoxelShadingProgram grassOverDirtFieldVoxelShadingProgram(
    const float depthStart, const float depthEnd,
    const float slopeStartDegrees, const float slopeEndDegrees,
    const float topRoughness, const float soilRoughness,
    const float hueVariation, const float brightnessVariation)
{
  using Opcode = VoxelShadingOpcode;
  VoxelShadingProgram program{};
  const float start = std::clamp(depthStart, 0.0f, 0.99f);
  const float end = std::clamp(depthEnd, start + 0.01f, 1.0f);
  const float slopeStart =
      std::clamp(slopeStartDegrees / 90.0f, 0.0f, 0.99f);
  const float slopeEnd =
      std::clamp(slopeEndDegrees / 90.0f, slopeStart + 0.01f, 1.0f);
  emit(program, Opcode::LoadSideOrRiser, 0u);
  emit(program, Opcode::LoadNormalizedDepth, 1u);
  emit(program, Opcode::Smoothstep, 1u, 1u, 0u, start, end);
  emit(program, Opcode::LoadTerrainSlope, 2u);
  emit(program, Opcode::Smoothstep, 2u, 2u, 0u, slopeStart, slopeEnd);
  emit(program, Opcode::Maximum, 0u, 0u, 1u);
  emit(program, Opcode::Maximum, 0u, 0u, 2u);
  // Flat exposed tops remain the primary grass layer. Generated grass and dirt
  // fields stay bound for custom graphs, but the default terrain graph must not
  // turn their sampling pattern into soil coverage. Depth, terrain slope, and
  // actual side/riser faces remain the only default soil selectors.
  // The bounded field controls authored hue strength. Stable microvoxel
  // randomness independently varies luminance around one, so colour variation
  // is not chroma-only and remains deterministic across frames and pages.
  emit(program, Opcode::LoadField, 3u, 2u);
  if (hueVariation != 0.0f)
    emit(program, Opcode::ScaleBias, 4u, 3u, 0u,
         std::clamp(hueVariation, -1.0f, 1.0f), 0.0f);
  if (brightnessVariation > 0.0f)
  {
    const float amplitude = std::clamp(brightnessVariation, 0.0f, 0.5f);
    emit(program, Opcode::LoadStableRandom, 6u);
    emit(program, Opcode::ScaleBias, 6u, 6u, 0u,
         amplitude * 2.0f, 1.0f - amplitude);
  }
  const float top = std::clamp(topRoughness, 0.0f, 1.0f);
  const float soil = std::clamp(soilRoughness, 0.0f, 1.0f);
  emit(program, Opcode::ScaleBias, 5u, 0u, 0u, soil - top, top);
  emit(program, Opcode::OutputLayerWeight, 0u, 0u);
  if (hueVariation != 0.0f)
    emit(program, Opcode::OutputHueVariation, 0u, 4u);
  if (brightnessVariation > 0.0f)
    emit(program, Opcode::OutputBrightness, 0u, 6u);
  emit(program, Opcode::OutputRoughness, 0u, 5u);
  return program;
}

} // namespace voxel::material
