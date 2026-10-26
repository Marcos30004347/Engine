#pragma once

#include "voxel/material/VoxelMaterialField.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::material
{

inline constexpr uint32_t kVoxelShadingRegisterCount = 8u;
inline constexpr uint32_t kMaximumVoxelShadingInstructionCount = 16u;
inline constexpr uint32_t kMaximumVoxelShadingMaterialFieldCount = 2u;
inline constexpr uint32_t kVoxelShadingPaletteColourCount = 4u;

// A deliberately small scalar IR for choosing the blend between a material's
// primary and secondary colours, applying bounded colour variation, and
// overriding PBR scalars. More outputs can be added without changing the stable
// material handle or the instruction encoding.
enum class VoxelShadingOpcode : uint32_t
{
  LoadNormalizedDepth = 0u,
  LoadStableRandom = 1u,
  LoadTerrainSlope = 2u,
  LoadConstant = 3u,
  Add = 4u,
  Multiply = 5u,
  OneMinus = 6u,
  Clamp01 = 7u,
  Smoothstep = 8u,
  OutputLayerWeight = 9u,
  OutputHueVariation = 10u,
  OutputBrightness = 11u,
  ScaleBias = 12u,
  LoadSideOrRiser = 13u,
  Maximum = 14u,
  OutputRoughness = 15u,
  OutputMetallic = 16u,
  // Strict comparison matches the legacy HardCrust profile exactly.
  GreaterThan = 17u,
  // Binary availability/classification input. Unlike LoadTerrainSlope this is
  // one for legacy slope payloads whose quantitative angle is unavailable.
  LoadTerrainSlopeClassified = 18u,
  LoadParameter = 19u,
  // source0 selects one of four normalized, pre-resolved material fields.
  LoadField = 20u,
  // Palette entries zero and one remain the per-voxel primary/secondary
  // colours. These outputs add bounded coverage for material-owned entries
  // two and three without changing the legacy two-colour instruction.
  OutputPaletteWeight2 = 21u,
  OutputPaletteWeight3 = 22u,
};

struct VoxelShadingInstruction
{
  VoxelShadingOpcode opcode = VoxelShadingOpcode::LoadConstant;
  uint32_t destination = 0u;
  uint32_t source0 = 0u;
  uint32_t source1 = 0u;
  float immediate0 = 0.0f;
  float immediate1 = 0.0f;

  bool operator==(const VoxelShadingInstruction &other) const;
  bool operator<(const VoxelShadingInstruction &other) const;
};

struct VoxelShadingProgram
{
  std::vector<VoxelShadingInstruction> instructions;

  bool operator==(const VoxelShadingProgram &other) const
  {
    return instructions == other.instructions;
  }

  bool operator!=(const VoxelShadingProgram &other) const
  {
    return !(*this == other);
  }

  bool operator<(const VoxelShadingProgram &other) const
  {
    return instructions < other.instructions;
  }
};

struct alignas(16) VoxelShadingInstructionGPU
{
  // opcode | destination << 8 | source0 << 16 | source1 << 24
  uint32_t opcodeOperands = 0u;
  float immediate0 = 0.0f;
  float immediate1 = 0.0f;
  uint32_t reserved = 0u;
};
static_assert(sizeof(VoxelShadingInstructionGPU) == 16u);

struct alignas(16) VoxelShadingProgramGPU
{
  uint32_t instructionOffset = 0u;
  uint32_t instructionCount = 0u;
  uint32_t flags = 0u;
  uint32_t reserved = 0u;
};
static_assert(sizeof(VoxelShadingProgramGPU) == 16u);

inline constexpr uint32_t kVoxelShadingProgramEnabled = 1u << 0u;
inline constexpr uint32_t kVoxelShadingProgramPaletteShift = 4u;
inline constexpr uint32_t kVoxelShadingProgramPaletteMask = 0x3u <<
    kVoxelShadingProgramPaletteShift;
inline constexpr uint32_t kVoxelShadingProgramMaterialFieldShift = 8u;
inline constexpr uint32_t kVoxelShadingProgramMaterialFieldMask =
    ((1u << kVoxelMaterialFieldCount) - 1u)
    << kVoxelShadingProgramMaterialFieldShift;

struct alignas(16) VoxelShadingParameterBlockGPU
{
  std::array<float, 4> values{};

  bool operator==(const VoxelShadingParameterBlockGPU &other) const
  {
    return values == other.values;
  }

  bool operator!=(const VoxelShadingParameterBlockGPU &other) const
  {
    return !(*this == other);
  }

  bool operator<(const VoxelShadingParameterBlockGPU &other) const
  {
    return values < other.values;
  }
};
static_assert(sizeof(VoxelShadingParameterBlockGPU) == 16u);

// The first two palette colours continue to come from the resolved voxel, so
// biome tinting and the compact brick ABI remain unchanged. Only the two
// auxiliary colours need one record per stable material handle.
struct alignas(16) VoxelShadingPaletteGPU
{
  std::array<uint32_t, 2> additionalAlbedoRGBA8{
    0xffffffffu, 0xffffffffu};
  std::array<uint32_t, 2> reserved{};

  bool operator==(const VoxelShadingPaletteGPU &other) const
  {
    return additionalAlbedoRGBA8 == other.additionalAlbedoRGBA8 &&
        reserved == other.reserved;
  }

  bool operator!=(const VoxelShadingPaletteGPU &other) const
  {
    return !(*this == other);
  }

  bool operator<(const VoxelShadingPaletteGPU &other) const
  {
    return additionalAlbedoRGBA8 < other.additionalAlbedoRGBA8 ||
        (additionalAlbedoRGBA8 == other.additionalAlbedoRGBA8 &&
         reserved < other.reserved);
  }
};
static_assert(sizeof(VoxelShadingPaletteGPU) == 16u);

struct VoxelShadingInputs
{
  float normalizedDepth = 0.0f;
  float stableRandom = 0.0f;
  // Normalized terrain slope angle: 0 is horizontal and 1 is vertical.
  float terrainSlope = 0.0f;
  bool sideOrRiser = false;
  bool terrainSlopeClassified = false;
  VoxelShadingParameterBlockGPU parameters{};
  std::array<float, kVoxelMaterialFieldCount> fields{};
};

struct VoxelShadingResult
{
  float layerWeight = 0.0f;
  // Normalized primary, secondary, tertiary, and quaternary contributions.
  // Existing programs produce {1-layerWeight, layerWeight, 0, 0} exactly.
  std::array<float, kVoxelShadingPaletteColourCount> paletteWeights{
    1.0f, 0.0f, 0.0f, 0.0f};
  float hueVariation = 0.0f;
  float brightness = 1.0f;
  // Negative means the program did not override the authored scalar.
  float roughness = -1.0f;
  float metallic = -1.0f;
};

std::array<float, 3> blendVoxelShadingPalette(
    const std::array<std::array<float, 3>, kVoxelShadingPaletteColourCount>
        &colours,
    const VoxelShadingResult &result);

bool verifyVoxelShadingProgram(
    const VoxelShadingProgram &program, std::string *error = nullptr);

bool voxelShadingProgramUsesTerrainSlope(
    const VoxelShadingProgram &program);

uint32_t voxelShadingRequiredMaterialFields(
    const VoxelShadingProgram &program);

uint32_t voxelShadingAdditionalPaletteMask(
    const VoxelShadingProgram &program);

bool compileVoxelShadingProgram(
    const VoxelShadingProgram &program,
    std::vector<VoxelShadingInstructionGPU> &instructions,
    std::string *error = nullptr);

bool evaluateVoxelShadingProgram(
    const VoxelShadingProgram &program, const VoxelShadingInputs &inputs,
    VoxelShadingResult &result, std::string *error = nullptr);

// Compatibility helper for callers interested only in the original scalar
// layer-weight result.
bool evaluateVoxelShadingProgram(
    const VoxelShadingProgram &program, const VoxelShadingInputs &inputs,
    float &layerWeight, std::string *error = nullptr);

} // namespace voxel::material
