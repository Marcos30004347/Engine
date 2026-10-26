#pragma once

#include "voxel/material/VoxelMaterialField.hpp"
#include "voxel/svraster/SparseVoxelMicroMaterial.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::material
{

// Geometry uses signed Q16.16 throughout. Integer arithmetic is part of the
// file/runtime contract so CPU materialization and WGSL evaluation make the
// same occupancy decision at every microcell boundary.
using VoxelGeometryFixed = int32_t;
inline constexpr VoxelGeometryFixed kVoxelGeometryFixedOne = 1 << 16;
inline constexpr uint32_t kVoxelGeometryRegisterCount = 8u;
inline constexpr uint32_t kMaximumVoxelGeometryInstructionCount = 24u;
inline constexpr uint32_t kMaximumVoxelGeometryMaterialFieldCount = 1u;

VoxelGeometryFixed voxelGeometryFixedFromDouble(double value);
double voxelGeometryFixedToDouble(VoxelGeometryFixed value);

enum class VoxelGeometryDomain : uint32_t
{
  Heightfield = 0u,
  Volume = 1u,
};

enum class VoxelGeometryExecution : uint32_t
{
  BuiltIn = 0u,
  Direct = 1u,
  CachedHeightfield = 2u,
  CachedVolume = 3u,
};

enum class VoxelGeometryExecutionHint : uint32_t
{
  Automatic = 0u,
  Direct = 1u,
  Cached = 2u,
};

enum class VoxelGeometryOpcode : uint32_t
{
  LoadBaseSurface = 0u,
  LoadWorldNoise = 1u,
  LoadConstant = 2u,
  LoadParameter = 3u,
  LoadContextMask = 4u,
  Add = 5u,
  Subtract = 6u,
  Multiply = 7u,
  Minimum = 8u,
  Maximum = 9u,
  OneMinus = 10u,
  Clamp01 = 11u,
  Quantize = 12u,
  Select = 13u,
  OutputRetainedHeight = 14u,
  OutputCarveDepth = 15u,
  OutputCoverage = 16u,
  // Phase-five scalar context reads. Each value is normalized Q16.16 and the
  // verifier folds the corresponding field into the program requirements.
  LoadMicroSlope = 17u,
  LoadTerrainSlope = 18u,
  LoadDepthBelowSurface = 19u,
  LoadTerrainBlend = 20u,
  LoadAuxiliary = 21u,
  Absolute = 22u,
  // Approved bounded-volume CSG. Coverage is treated as a [0, 1] occupancy
  // field: union=max(a,b), intersection=min(a,b), subtraction=min(a,1-b).
  VolumeUnion = 23u,
  VolumeIntersection = 24u,
  VolumeSubtract = 25u,
  // source0 selects one of four normalized, pre-resolved material fields.
  LoadField = 26u,
};

struct VoxelGeometryInstruction
{
  VoxelGeometryOpcode opcode = VoxelGeometryOpcode::LoadConstant;
  uint32_t destination = 0u;
  uint32_t source0 = 0u;
  uint32_t source1 = 0u;
  VoxelGeometryFixed immediate = 0;

  bool operator==(const VoxelGeometryInstruction &other) const;
  bool operator<(const VoxelGeometryInstruction &other) const;
};

struct VoxelGeometryProgram
{
  VoxelGeometryDomain domain = VoxelGeometryDomain::Heightfield;
  VoxelGeometryExecutionHint executionHint =
      VoxelGeometryExecutionHint::Automatic;
  std::vector<VoxelGeometryInstruction> instructions;

  bool operator==(const VoxelGeometryProgram &other) const;
  bool operator!=(const VoxelGeometryProgram &other) const
  {
    return !(*this == other);
  }
  bool operator<(const VoxelGeometryProgram &other) const;
};

struct alignas(16) VoxelGeometryInstructionGPU
{
  // opcode | destination << 8 | source0 << 16 | source1 << 24
  uint32_t opcodeOperands = 0u;
  int32_t immediate = 0;
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;
};
static_assert(sizeof(VoxelGeometryInstructionGPU) == 16u);

struct alignas(16) VoxelGeometryProgramGPU
{
  uint32_t instructionOffset = 0u;
  uint32_t instructionCount = 0u;
  uint32_t executionFlags = 0u;
  uint32_t builtInProfile = 0u;
};
static_assert(sizeof(VoxelGeometryProgramGPU) == 16u);

inline constexpr uint32_t kVoxelGeometryProgramEnabled = 1u << 0u;
inline constexpr uint32_t kVoxelGeometryProgramDomainVolume = 1u << 1u;
inline constexpr uint32_t kVoxelGeometryProgramExecutionShift = 2u;
inline constexpr uint32_t kVoxelGeometryProgramMaterialFieldShift = 8u;
inline constexpr uint32_t kVoxelGeometryProgramMaterialFieldMask =
    ((1u << kVoxelMaterialFieldCount) - 1u)
    << kVoxelGeometryProgramMaterialFieldShift;

struct alignas(16) VoxelGeometryParameterBlockGPU
{
  std::array<int32_t, 4> values{};

  bool operator==(const VoxelGeometryParameterBlockGPU &other) const
  {
    return values == other.values;
  }
  bool operator!=(const VoxelGeometryParameterBlockGPU &other) const
  {
    return !(*this == other);
  }
  bool operator<(const VoxelGeometryParameterBlockGPU &other) const
  {
    return values < other.values;
  }
};
static_assert(sizeof(VoxelGeometryParameterBlockGPU) == 16u);

struct VoxelGeometryInputs
{
  std::array<int32_t, 3> parentKey{};
  std::array<uint32_t, 2> faceCell{};
  std::array<uint32_t, 3> volumeCell{};
  uint32_t face = 0u;
  uint32_t subdivision = 1u;
  uint32_t materialSeed = 0u;
  uint32_t context = 0u;
  VoxelGeometryFixed baseSurface = kVoxelGeometryFixedOne;
  VoxelGeometryFixed microSlope = 0;
  VoxelGeometryFixed terrainSlope = 0;
  VoxelGeometryFixed depthBelowSurface = 0;
  VoxelGeometryFixed terrainBlend = 0;
  VoxelGeometryFixed auxiliary = 0;
  VoxelGeometryParameterBlockGPU parameters{};
  std::array<VoxelGeometryFixed, kVoxelMaterialFieldCount> fields{};
};

struct VoxelGeometryResult
{
  VoxelGeometryFixed retainedHeight = kVoxelGeometryFixedOne;
  VoxelGeometryFixed carveDepth = 0;
  VoxelGeometryFixed coverage = kVoxelGeometryFixedOne;
};

// Decodes the page-local VoxelAuxContext packed ABI without coupling Geometry
// IR to a particular page container.
void applyPackedVoxelAuxContext(
    uint32_t packedContext, VoxelGeometryInputs &inputs);

struct VoxelGeometryCompilation
{
  VoxelGeometryExecution execution = VoxelGeometryExecution::BuiltIn;
  VoxelGeometryProgramGPU gpu{};
  std::vector<VoxelGeometryInstructionGPU> instructions;
};

struct VoxelGeometryHeightfield
{
  uint32_t subdivision = 0u;
  // Row-major face-cell retained heights, materialized once per parent/face.
  std::vector<VoxelGeometryFixed> retainedHeights;
};

struct VoxelGeometryVolume
{
  uint32_t subdivision = 0u;
  // X-major linear occupancy, materialized once per canonical parent.
  std::vector<uint8_t> occupancy;
};

bool verifyVoxelGeometryProgram(
    const VoxelGeometryProgram &program, std::string *error = nullptr);
bool voxelGeometryProgramUsesBaseSurface(const VoxelGeometryProgram &program);
bool voxelGeometryProgramUsesContext(const VoxelGeometryProgram &program);
uint32_t voxelGeometryRequiredContextFields(
    const VoxelGeometryProgram &program);
uint32_t voxelGeometryRequiredMaterialFields(
    const VoxelGeometryProgram &program);
bool compileVoxelGeometryProgram(
    const VoxelGeometryProgram &program, uint32_t builtInProfile,
    VoxelGeometryCompilation &result, std::string *error = nullptr);
bool evaluateVoxelGeometryProgram(
    const VoxelGeometryProgram &program, const VoxelGeometryInputs &inputs,
    VoxelGeometryResult &result, std::string *error = nullptr);
bool evaluateCompiledVoxelGeometryProgram(
    const VoxelGeometryProgramGPU &program,
    const std::vector<VoxelGeometryInstructionGPU> &instructions,
    const VoxelGeometryInputs &inputs, VoxelGeometryResult &result,
    std::string *error = nullptr);
bool materializeVoxelGeometryHeightfield(
    const VoxelGeometryProgram &program, const VoxelGeometryInputs &parentInputs,
    VoxelGeometryHeightfield &result, std::string *error = nullptr);
bool voxelGeometryCellOccupied(
    const VoxelGeometryProgram &program, const VoxelGeometryInputs &inputs,
    const std::array<uint32_t, 3> &cell, bool &occupied,
    std::string *error = nullptr);
bool materializeVoxelGeometryVolume(
    const VoxelGeometryProgram &program, const VoxelGeometryInputs &parentInputs,
    VoxelGeometryVolume &result, std::string *error = nullptr);

// Stable band-limit used once exact microcells are sub-pixel. Heightfields use
// their analytic column average; volume programs use a fixed stratified grid.
VoxelGeometryFixed voxelGeometryExpectedCoverage(
    const VoxelGeometryProgram &program, const VoxelGeometryInputs &inputs,
    uint32_t strata = 4u);

// Empty programs are specialized built-ins. This converts every shipping
// legacy profile into the same program record/execution-selection contract.
VoxelGeometryCompilation compileVoxelGeometryBuiltIn(
    const svraster::SparseVoxelMicroDescriptor &descriptor);

// Phase-five compatibility replacement for the legacy volume-density switch.
// The same stable noise threshold is expressed through verified Volume IR.
VoxelGeometryProgram volumeDensityVoxelGeometryProgram(
    VoxelGeometryFixed densityThreshold);

} // namespace voxel::material
