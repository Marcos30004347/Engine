#include "voxel/material/VoxelGeometryIR.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>

namespace voxel::material
{
namespace
{

bool fail(std::string *error, const char *message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool failNode(
    std::string *error, const size_t index, const char *constraint)
{
  if (error != nullptr)
    *error = "Geometry node " + std::to_string(index) +
        " violates constraint: " + constraint;
  return false;
}

int32_t saturate64(const int64_t value)
{
  return static_cast<int32_t>(std::clamp<int64_t>(
      value, std::numeric_limits<int32_t>::min(),
      std::numeric_limits<int32_t>::max()));
}

int32_t clampGeometry(const int64_t value)
{
  return static_cast<int32_t>(std::clamp<int64_t>(
      value, -kVoxelGeometryFixedOne, kVoxelGeometryFixedOne));
}

uint32_t mix(uint32_t value)
{
  value ^= value >> 16u;
  value *= 0x7feb352du;
  value ^= value >> 15u;
  value *= 0x846ca68bu;
  return value ^ (value >> 16u);
}

uint32_t worldNoise(const VoxelGeometryInputs &inputs, const uint32_t salt)
{
  // Face-cell coordinates are first lifted into canonical world microcell
  // space. Adjacent parents therefore hash their shared boundary identically.
  const uint32_t subdivision = std::max(inputs.subdivision, 1u);
  if (inputs.face >= 6u)
  {
    std::array<uint32_t, 3> coordinate{
      static_cast<uint32_t>(inputs.parentKey[0]) * subdivision +
          inputs.volumeCell[0],
      static_cast<uint32_t>(inputs.parentKey[1]) * subdivision +
          inputs.volumeCell[1],
      static_cast<uint32_t>(inputs.parentKey[2]) * subdivision +
          inputs.volumeCell[2],
    };
    uint32_t value = inputs.materialSeed ^ salt;
    value = mix(value ^ coordinate[0] * 0x8da6b343u);
    value = mix(value ^ coordinate[1] * 0xd8163841u);
    value = mix(value ^ coordinate[2] * 0xcb1ab31fu);
    return mix(value ^ 0x517cc1b7u);
  }
  const uint32_t axis = (inputs.face >> 1u) % 3u;
  const uint32_t uAxis = axis == 0u ? 2u : 0u;
  const uint32_t vAxis = axis == 1u ? 2u : 1u;
  std::array<uint32_t, 3> coordinate{
    static_cast<uint32_t>(inputs.parentKey[0]) * subdivision,
    static_cast<uint32_t>(inputs.parentKey[1]) * subdivision,
    static_cast<uint32_t>(inputs.parentKey[2]) * subdivision,
  };
  coordinate[uAxis] += inputs.faceCell[0];
  coordinate[vAxis] += inputs.faceCell[1];
  if ((inputs.face & 1u) != 0u)
    coordinate[axis] += subdivision;
  uint32_t value = inputs.materialSeed ^ salt;
  value = mix(value ^ coordinate[0] * 0x8da6b343u);
  value = mix(value ^ coordinate[1] * 0xd8163841u);
  value = mix(value ^ coordinate[2] * 0xcb1ab31fu);
  return mix(value ^ inputs.face * 0x85ebca6bu);
}

bool isOutput(const VoxelGeometryOpcode opcode)
{
  return opcode == VoxelGeometryOpcode::OutputRetainedHeight ||
      opcode == VoxelGeometryOpcode::OutputCarveDepth ||
      opcode == VoxelGeometryOpcode::OutputCoverage;
}

bool readsSource0(const VoxelGeometryOpcode opcode)
{
  switch (opcode)
  {
  case VoxelGeometryOpcode::Add:
  case VoxelGeometryOpcode::Subtract:
  case VoxelGeometryOpcode::Multiply:
  case VoxelGeometryOpcode::Minimum:
  case VoxelGeometryOpcode::Maximum:
  case VoxelGeometryOpcode::OneMinus:
  case VoxelGeometryOpcode::Clamp01:
  case VoxelGeometryOpcode::Quantize:
  case VoxelGeometryOpcode::Select:
  case VoxelGeometryOpcode::OutputRetainedHeight:
  case VoxelGeometryOpcode::OutputCarveDepth:
  case VoxelGeometryOpcode::OutputCoverage:
  case VoxelGeometryOpcode::Absolute:
  case VoxelGeometryOpcode::VolumeUnion:
  case VoxelGeometryOpcode::VolumeIntersection:
  case VoxelGeometryOpcode::VolumeSubtract:
    return true;
  default:
    return false;
  }
}

bool readsSource1(const VoxelGeometryOpcode opcode)
{
  return opcode == VoxelGeometryOpcode::Add ||
      opcode == VoxelGeometryOpcode::Subtract ||
      opcode == VoxelGeometryOpcode::Multiply ||
      opcode == VoxelGeometryOpcode::Minimum ||
      opcode == VoxelGeometryOpcode::Maximum ||
      opcode == VoxelGeometryOpcode::Select ||
      opcode == VoxelGeometryOpcode::VolumeUnion ||
      opcode == VoxelGeometryOpcode::VolumeIntersection ||
      opcode == VoxelGeometryOpcode::VolumeSubtract;
}

bool isVolumeCSG(const VoxelGeometryOpcode opcode)
{
  return opcode == VoxelGeometryOpcode::VolumeUnion ||
      opcode == VoxelGeometryOpcode::VolumeIntersection ||
      opcode == VoxelGeometryOpcode::VolumeSubtract;
}

} // namespace

VoxelGeometryFixed voxelGeometryFixedFromDouble(const double value)
{
  if (!std::isfinite(value))
    return 0;
  return saturate64(static_cast<int64_t>(std::llround(
      value * static_cast<double>(kVoxelGeometryFixedOne))));
}

double voxelGeometryFixedToDouble(const VoxelGeometryFixed value)
{
  return static_cast<double>(value) /
      static_cast<double>(kVoxelGeometryFixedOne);
}

void applyPackedVoxelAuxContext(
    const uint32_t packedContext, VoxelGeometryInputs &inputs)
{
  const auto fixedUNorm8 = [](const uint32_t value)
  {
    return static_cast<VoxelGeometryFixed>(
        (value * static_cast<uint32_t>(kVoxelGeometryFixedOne) + 127u) /
        255u);
  };
  inputs.terrainSlope = fixedUNorm8(packedContext & 0xffu);
  inputs.depthBelowSurface = fixedUNorm8((packedContext >> 8u) & 0xffu);
  inputs.terrainBlend = fixedUNorm8((packedContext >> 16u) & 0xffu);
  inputs.auxiliary = fixedUNorm8(packedContext >> 24u);
}

bool VoxelGeometryInstruction::operator==(
    const VoxelGeometryInstruction &other) const
{
  return std::tie(opcode, destination, source0, source1, immediate) ==
      std::tie(other.opcode, other.destination, other.source0,
               other.source1, other.immediate);
}

bool VoxelGeometryInstruction::operator<(
    const VoxelGeometryInstruction &other) const
{
  return std::tie(opcode, destination, source0, source1, immediate) <
      std::tie(other.opcode, other.destination, other.source0,
               other.source1, other.immediate);
}

bool VoxelGeometryProgram::operator==(
    const VoxelGeometryProgram &other) const
{
  return domain == other.domain && executionHint == other.executionHint &&
      instructions == other.instructions;
}

bool VoxelGeometryProgram::operator<(
    const VoxelGeometryProgram &other) const
{
  return std::tie(domain, executionHint, instructions) <
      std::tie(other.domain, other.executionHint, other.instructions);
}

bool verifyVoxelGeometryProgram(
    const VoxelGeometryProgram &program, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (program.instructions.empty())
    return true;
  if (program.instructions.size() > kMaximumVoxelGeometryInstructionCount)
    return fail(error, "Voxel geometry program exceeds the instruction limit");

  std::array<bool, kVoxelGeometryRegisterCount> initialized{};
  bool outputPhase = false;
  uint32_t geometryOutputs = 0u;
  uint32_t coverageOutputs = 0u;
  for (size_t nodeIndex = 0u; nodeIndex < program.instructions.size();
       ++nodeIndex)
  {
    const VoxelGeometryInstruction &instruction =
        program.instructions[nodeIndex];
    if (static_cast<uint32_t>(instruction.opcode) >
        static_cast<uint32_t>(VoxelGeometryOpcode::LoadField))
      return failNode(error, nodeIndex, "unsupported opcode");
    if (!isOutput(instruction.opcode) &&
        instruction.destination >= kVoxelGeometryRegisterCount)
      return failNode(error, nodeIndex, "destination register is out of range");
    if (instruction.opcode == VoxelGeometryOpcode::LoadParameter &&
        instruction.source0 >= 4u)
      return failNode(error, nodeIndex, "parameter index is out of range");
    if (instruction.opcode == VoxelGeometryOpcode::LoadField &&
        instruction.source0 >= kVoxelMaterialFieldCount)
      return failNode(error, nodeIndex, "material field index is out of range");
    if (instruction.opcode == VoxelGeometryOpcode::LoadContextMask &&
        instruction.source0 == 0u)
      return failNode(error, nodeIndex, "context read has no declared field");
    if (isVolumeCSG(instruction.opcode) &&
        program.domain != VoxelGeometryDomain::Volume)
      return failNode(error, nodeIndex, "volume CSG used in heightfield domain");
    if (readsSource0(instruction.opcode) &&
        (instruction.source0 >= kVoxelGeometryRegisterCount ||
         !initialized[instruction.source0]))
      return failNode(error, nodeIndex, "source register is uninitialized");
    if (readsSource1(instruction.opcode) &&
        (instruction.source1 >= kVoxelGeometryRegisterCount ||
         !initialized[instruction.source1]))
      return failNode(error, nodeIndex, "second source register is uninitialized");
    if (instruction.opcode == VoxelGeometryOpcode::Quantize &&
        (instruction.immediate <= 0 ||
         instruction.immediate > kVoxelGeometryFixedOne))
      return failNode(error, nodeIndex, "quantization step is outside (0, 1]");
    if ((instruction.opcode == VoxelGeometryOpcode::LoadConstant ||
         instruction.opcode == VoxelGeometryOpcode::Select) &&
        (instruction.immediate < -kVoxelGeometryFixedOne ||
         instruction.immediate > kVoxelGeometryFixedOne))
      return failNode(error, nodeIndex, "constant exceeds fixed-point range");
    if (isOutput(instruction.opcode))
    {
      outputPhase = true;
      if (instruction.opcode == VoxelGeometryOpcode::OutputCoverage)
        ++coverageOutputs;
      else
        ++geometryOutputs;
    }
    else
    {
      if (outputPhase)
        return failNode(error, nodeIndex, "calculation follows an output node");
      initialized[instruction.destination] = true;
    }
  }
  if (program.domain == VoxelGeometryDomain::Heightfield &&
      geometryOutputs != 1u)
    return fail(error, "Voxel heightfield program must have one geometry output");
  if (program.domain == VoxelGeometryDomain::Volume &&
      (geometryOutputs > 1u || coverageOutputs != 1u))
    return fail(error, "Voxel volume program must have one coverage output");
  if (coverageOutputs > 1u)
    return fail(error, "Voxel geometry program has duplicate coverage output");
  const uint32_t materialFields =
      voxelGeometryRequiredMaterialFields(program);
  if (materialFields != 0u && (materialFields & (materialFields - 1u)) != 0u)
    return fail(error, "Voxel geometry program exceeds the material field limit");
  return true;
}

bool voxelGeometryProgramUsesBaseSurface(const VoxelGeometryProgram &program)
{
  return std::any_of(
      program.instructions.begin(), program.instructions.end(),
      [](const VoxelGeometryInstruction &instruction)
      { return instruction.opcode == VoxelGeometryOpcode::LoadBaseSurface; });
}

bool voxelGeometryProgramUsesContext(const VoxelGeometryProgram &program)
{
  return std::any_of(
      program.instructions.begin(), program.instructions.end(),
      [](const VoxelGeometryInstruction &instruction)
      { return instruction.opcode == VoxelGeometryOpcode::LoadContextMask; });
}

uint32_t voxelGeometryRequiredContextFields(
    const VoxelGeometryProgram &program)
{
  // Bit zero is VoxelMaterialContextField::BaseSurface. LoadContextMask keeps
  // its raw packed-context mask in immediate and declares the material context
  // field(s) responsible for those bits in source0.
  uint32_t result = voxelGeometryProgramUsesBaseSurface(program) ? 1u : 0u;
  for (const VoxelGeometryInstruction &instruction : program.instructions)
  {
    if (instruction.opcode == VoxelGeometryOpcode::LoadContextMask)
      result |= instruction.source0;
    else if (instruction.opcode == VoxelGeometryOpcode::LoadMicroSlope)
      result |= 1u << 1u;
    else if (instruction.opcode == VoxelGeometryOpcode::LoadTerrainSlope)
      result |= 1u << 2u;
    else if (instruction.opcode == VoxelGeometryOpcode::LoadDepthBelowSurface)
      result |= 1u << 3u;
    else if (instruction.opcode == VoxelGeometryOpcode::LoadTerrainBlend)
      result |= 1u << 4u;
    else if (instruction.opcode == VoxelGeometryOpcode::LoadAuxiliary)
      result |= 1u << 5u;
  }
  return result;
}

uint32_t voxelGeometryRequiredMaterialFields(
    const VoxelGeometryProgram &program)
{
  uint32_t result = 0u;
  for (const VoxelGeometryInstruction &instruction : program.instructions)
    if (instruction.opcode == VoxelGeometryOpcode::LoadField &&
        instruction.source0 < kVoxelMaterialFieldCount)
      result |= 1u << instruction.source0;
  return result;
}

bool compileVoxelGeometryProgram(
    const VoxelGeometryProgram &program, const uint32_t builtInProfile,
    VoxelGeometryCompilation &result, std::string *error)
{
  result = {};
  if (!verifyVoxelGeometryProgram(program, error))
    return false;
  if (program.instructions.empty())
  {
    result.execution = VoxelGeometryExecution::BuiltIn;
    result.gpu.builtInProfile = builtInProfile;
    return true;
  }
  const uint32_t materialFields =
      voxelGeometryRequiredMaterialFields(program);
  // Texture-backed geometry is never interpreted against live residency.
  // It is materialized once into the shared deterministic parent cache.
  if (materialFields != 0u)
    result.execution = program.domain == VoxelGeometryDomain::Volume
        ? VoxelGeometryExecution::CachedVolume
        : VoxelGeometryExecution::CachedHeightfield;
  else if (program.executionHint == VoxelGeometryExecutionHint::Direct)
    result.execution = VoxelGeometryExecution::Direct;
  else if (program.domain == VoxelGeometryDomain::Volume)
    result.execution = VoxelGeometryExecution::CachedVolume;
  else if (program.executionHint == VoxelGeometryExecutionHint::Cached ||
           program.instructions.size() > 8u ||
           voxelGeometryProgramUsesBaseSurface(program))
    result.execution = VoxelGeometryExecution::CachedHeightfield;
  else
    result.execution = VoxelGeometryExecution::Direct;

  result.instructions.reserve(program.instructions.size());
  for (const VoxelGeometryInstruction &instruction : program.instructions)
  {
    result.instructions.push_back({
      (static_cast<uint32_t>(instruction.opcode) & 0xffu) |
          ((instruction.destination & 0xffu) << 8u) |
          ((instruction.source0 & 0xffu) << 16u) |
          ((instruction.source1 & 0xffu) << 24u),
      instruction.immediate, 0u, 0u});
  }
  result.gpu.instructionCount =
      static_cast<uint32_t>(result.instructions.size());
  result.gpu.executionFlags = kVoxelGeometryProgramEnabled |
      (program.domain == VoxelGeometryDomain::Volume
           ? kVoxelGeometryProgramDomainVolume : 0u) |
      (static_cast<uint32_t>(result.execution) <<
       kVoxelGeometryProgramExecutionShift) |
      (materialFields
       << kVoxelGeometryProgramMaterialFieldShift);
  result.gpu.builtInProfile = builtInProfile;
  return true;
}

bool evaluateVoxelGeometryProgram(
    const VoxelGeometryProgram &program, const VoxelGeometryInputs &inputs,
    VoxelGeometryResult &result, std::string *error)
{
  result = {};
  if (!verifyVoxelGeometryProgram(program, error))
    return false;
  if (program.instructions.empty())
    return true;
  std::array<int32_t, kVoxelGeometryRegisterCount> registers{};
  for (const VoxelGeometryInstruction &instruction : program.instructions)
  {
    const int32_t a = instruction.source0 < registers.size()
        ? registers[instruction.source0] : 0;
    const int32_t b = instruction.source1 < registers.size()
        ? registers[instruction.source1] : 0;
    switch (instruction.opcode)
    {
    case VoxelGeometryOpcode::LoadBaseSurface:
      registers[instruction.destination] = std::clamp(
          inputs.baseSurface, 0, kVoxelGeometryFixedOne);
      break;
    case VoxelGeometryOpcode::LoadWorldNoise:
      registers[instruction.destination] = static_cast<int32_t>(
          (static_cast<uint64_t>(worldNoise(
               inputs, static_cast<uint32_t>(instruction.immediate))) *
           kVoxelGeometryFixedOne) >> 32u);
      break;
    case VoxelGeometryOpcode::LoadConstant:
      registers[instruction.destination] = instruction.immediate;
      break;
    case VoxelGeometryOpcode::LoadParameter:
      registers[instruction.destination] = std::clamp(
          inputs.parameters.values[instruction.source0],
          -kVoxelGeometryFixedOne, kVoxelGeometryFixedOne);
      break;
    case VoxelGeometryOpcode::LoadContextMask:
      registers[instruction.destination] =
          (inputs.context & static_cast<uint32_t>(instruction.immediate)) != 0u
          ? kVoxelGeometryFixedOne : 0;
      break;
    case VoxelGeometryOpcode::Add:
      registers[instruction.destination] = clampGeometry(int64_t{a} + b);
      break;
    case VoxelGeometryOpcode::Subtract:
      registers[instruction.destination] = clampGeometry(int64_t{a} - b);
      break;
    case VoxelGeometryOpcode::Multiply:
      registers[instruction.destination] = clampGeometry(
          (int64_t{a} * b) / kVoxelGeometryFixedOne);
      break;
    case VoxelGeometryOpcode::Minimum:
      registers[instruction.destination] = std::min(a, b);
      break;
    case VoxelGeometryOpcode::Maximum:
      registers[instruction.destination] = std::max(a, b);
      break;
    case VoxelGeometryOpcode::OneMinus:
      registers[instruction.destination] = clampGeometry(
          int64_t{kVoxelGeometryFixedOne} - a);
      break;
    case VoxelGeometryOpcode::Clamp01:
      registers[instruction.destination] = std::clamp(
          a, 0, kVoxelGeometryFixedOne);
      break;
    case VoxelGeometryOpcode::Quantize:
      registers[instruction.destination] =
          ((a + instruction.immediate / 2) / instruction.immediate) *
          instruction.immediate;
      break;
    case VoxelGeometryOpcode::Select:
      registers[instruction.destination] = a != 0 ? b : instruction.immediate;
      break;
    case VoxelGeometryOpcode::OutputRetainedHeight:
      result.retainedHeight = std::clamp(a, 0, kVoxelGeometryFixedOne);
      result.carveDepth = std::max(0, kVoxelGeometryFixedOne - result.retainedHeight);
      break;
    case VoxelGeometryOpcode::OutputCarveDepth:
      result.carveDepth = std::max(a, 0);
      result.retainedHeight = std::clamp(
          kVoxelGeometryFixedOne - a, 0, kVoxelGeometryFixedOne);
      break;
    case VoxelGeometryOpcode::OutputCoverage:
      result.coverage = std::clamp(a, 0, kVoxelGeometryFixedOne);
      break;
    case VoxelGeometryOpcode::LoadMicroSlope:
      registers[instruction.destination] = std::clamp(
          inputs.microSlope, 0, kVoxelGeometryFixedOne);
      break;
    case VoxelGeometryOpcode::LoadTerrainSlope:
      registers[instruction.destination] = std::clamp(
          inputs.terrainSlope, 0, kVoxelGeometryFixedOne);
      break;
    case VoxelGeometryOpcode::LoadDepthBelowSurface:
      registers[instruction.destination] = std::clamp(
          inputs.depthBelowSurface, 0, kVoxelGeometryFixedOne);
      break;
    case VoxelGeometryOpcode::LoadTerrainBlend:
      registers[instruction.destination] = std::clamp(
          inputs.terrainBlend, 0, kVoxelGeometryFixedOne);
      break;
    case VoxelGeometryOpcode::LoadAuxiliary:
      registers[instruction.destination] = std::clamp(
          inputs.auxiliary, 0, kVoxelGeometryFixedOne);
      break;
    case VoxelGeometryOpcode::Absolute:
      registers[instruction.destination] = a == std::numeric_limits<int32_t>::min()
          ? std::numeric_limits<int32_t>::max() : std::abs(a);
      break;
    case VoxelGeometryOpcode::VolumeUnion:
      registers[instruction.destination] = std::max(a, b);
      break;
    case VoxelGeometryOpcode::VolumeIntersection:
      registers[instruction.destination] = std::min(a, b);
      break;
    case VoxelGeometryOpcode::VolumeSubtract:
      registers[instruction.destination] = std::min(
          a, kVoxelGeometryFixedOne - std::clamp(b, 0, kVoxelGeometryFixedOne));
      break;
    case VoxelGeometryOpcode::LoadField:
      registers[instruction.destination] = std::clamp(
          inputs.fields[instruction.source0], 0, kVoxelGeometryFixedOne);
      break;
    }
  }
  return true;
}

bool evaluateCompiledVoxelGeometryProgram(
    const VoxelGeometryProgramGPU &program,
    const std::vector<VoxelGeometryInstructionGPU> &instructions,
    const VoxelGeometryInputs &inputs, VoxelGeometryResult &result,
    std::string *error)
{
  result = {};
  if ((program.executionFlags & kVoxelGeometryProgramEnabled) == 0u)
    return true;
  const uint64_t end = static_cast<uint64_t>(program.instructionOffset) +
      program.instructionCount;
  if (program.instructionCount > kMaximumVoxelGeometryInstructionCount ||
      end > instructions.size())
    return fail(error, "Compiled voxel geometry program range is invalid");
  VoxelGeometryProgram unpacked{};
  unpacked.domain =
      (program.executionFlags & kVoxelGeometryProgramDomainVolume) != 0u
      ? VoxelGeometryDomain::Volume : VoxelGeometryDomain::Heightfield;
  unpacked.instructions.reserve(program.instructionCount);
  for (uint32_t index = 0u; index < program.instructionCount; ++index)
  {
    const VoxelGeometryInstructionGPU &packed =
        instructions[program.instructionOffset + index];
    unpacked.instructions.push_back({
      static_cast<VoxelGeometryOpcode>(packed.opcodeOperands & 0xffu),
      (packed.opcodeOperands >> 8u) & 0xffu,
      (packed.opcodeOperands >> 16u) & 0xffu,
      packed.opcodeOperands >> 24u,
      packed.immediate});
  }
  return evaluateVoxelGeometryProgram(unpacked, inputs, result, error);
}

bool materializeVoxelGeometryHeightfield(
    const VoxelGeometryProgram &program,
    const VoxelGeometryInputs &parentInputs,
    VoxelGeometryHeightfield &result, std::string *error)
{
  result = {};
  if (!verifyVoxelGeometryProgram(program, error) ||
      program.domain != VoxelGeometryDomain::Heightfield)
    return program.domain == VoxelGeometryDomain::Heightfield;
  result.subdivision = std::max(parentInputs.subdivision, 1u);
  result.retainedHeights.resize(
      static_cast<size_t>(result.subdivision) * result.subdivision);
  for (uint32_t v = 0u; v < result.subdivision; ++v)
    for (uint32_t u = 0u; u < result.subdivision; ++u)
    {
      VoxelGeometryInputs inputs = parentInputs;
      inputs.faceCell = {u, v};
      VoxelGeometryResult sample{};
      if (!evaluateVoxelGeometryProgram(program, inputs, sample, error))
        return false;
      result.retainedHeights[v * result.subdivision + u] =
          sample.retainedHeight;
    }
  return true;
}

bool voxelGeometryCellOccupied(
    const VoxelGeometryProgram &program, const VoxelGeometryInputs &inputs,
    const std::array<uint32_t, 3> &cell, bool &occupied,
    std::string *error)
{
  occupied = false;
  const uint32_t subdivision = std::max(inputs.subdivision, 1u);
  if (cell[0] >= subdivision || cell[1] >= subdivision ||
      cell[2] >= subdivision)
    return fail(error, "Voxel geometry cell is out of range");
  VoxelGeometryInputs sample = inputs;
  sample.volumeCell = cell;
  if (program.domain == VoxelGeometryDomain::Volume)
    sample.face = 6u;
  VoxelGeometryResult result{};
  if (!evaluateVoxelGeometryProgram(program, sample, result, error))
    return false;
  if (program.domain == VoxelGeometryDomain::Volume)
  {
    occupied = result.coverage > 0;
    return true;
  }
  const uint32_t axis = (sample.face >> 1u) % 3u;
  const int64_t cellCentre =
      (static_cast<int64_t>(cell[axis]) * 2 + 1) *
      kVoxelGeometryFixedOne / (2 * subdivision);
  occupied = (sample.face & 1u) != 0u
      ? cellCentre <= result.retainedHeight
      : cellCentre >= kVoxelGeometryFixedOne - result.retainedHeight;
  return true;
}

bool materializeVoxelGeometryVolume(
    const VoxelGeometryProgram &program,
    const VoxelGeometryInputs &parentInputs,
    VoxelGeometryVolume &result, std::string *error)
{
  result = {};
  if (!verifyVoxelGeometryProgram(program, error) ||
      program.domain != VoxelGeometryDomain::Volume)
    return program.domain == VoxelGeometryDomain::Volume;
  result.subdivision = std::max(parentInputs.subdivision, 1u);
  result.occupancy.resize(
      static_cast<size_t>(result.subdivision) * result.subdivision *
      result.subdivision);
  for (uint32_t z = 0u; z < result.subdivision; ++z)
    for (uint32_t y = 0u; y < result.subdivision; ++y)
      for (uint32_t x = 0u; x < result.subdivision; ++x)
      {
        bool occupied = false;
        if (!voxelGeometryCellOccupied(
                program, parentInputs, {x, y, z}, occupied, error))
          return false;
        result.occupancy[
            (z * result.subdivision + y) * result.subdivision + x] =
            occupied ? 1u : 0u;
      }
  return true;
}

VoxelGeometryFixed voxelGeometryExpectedCoverage(
    const VoxelGeometryProgram &program, const VoxelGeometryInputs &inputs,
    const uint32_t strata)
{
  const uint32_t count = std::max(strata, 1u);
  int64_t total = 0;
  uint32_t samples = 0u;
  if (program.domain == VoxelGeometryDomain::Volume)
  {
    for (uint32_t z = 0u; z < count; ++z)
      for (uint32_t y = 0u; y < count; ++y)
        for (uint32_t x = 0u; x < count; ++x)
        {
          VoxelGeometryInputs sampleInputs = inputs;
          sampleInputs.face = 6u;
          sampleInputs.volumeCell = {
            x * std::max(inputs.subdivision, 1u) / count,
            y * std::max(inputs.subdivision, 1u) / count,
            z * std::max(inputs.subdivision, 1u) / count};
          VoxelGeometryResult result{};
          if (evaluateVoxelGeometryProgram(
                  program, sampleInputs, result, nullptr))
          {
            total += result.coverage;
            ++samples;
          }
        }
    return samples == 0u ? kVoxelGeometryFixedOne
                         : static_cast<int32_t>(total / samples);
  }
  for (uint32_t v = 0u; v < count; ++v)
    for (uint32_t u = 0u; u < count; ++u)
    {
      VoxelGeometryInputs sampleInputs = inputs;
      sampleInputs.faceCell = {
        u * std::max(inputs.subdivision, 1u) / count,
        v * std::max(inputs.subdivision, 1u) / count};
      VoxelGeometryResult result{};
      if (evaluateVoxelGeometryProgram(program, sampleInputs, result, nullptr))
      {
        total += program.domain == VoxelGeometryDomain::Heightfield
            ? static_cast<int64_t>(result.retainedHeight) * result.coverage /
                kVoxelGeometryFixedOne
            : result.coverage;
        ++samples;
      }
    }
  return samples == 0u ? kVoxelGeometryFixedOne
                       : static_cast<int32_t>(total / samples);
}

VoxelGeometryCompilation compileVoxelGeometryBuiltIn(
    const svraster::SparseVoxelMicroDescriptor &descriptor)
{
  VoxelGeometryCompilation result{};
  result.execution = VoxelGeometryExecution::BuiltIn;
  result.gpu.builtInProfile =
      static_cast<uint32_t>(descriptor.geometryProfile);
  result.gpu.executionFlags =
      descriptor.geometryProfile ==
              svraster::SparseVoxelMicroGeometryProfile::VolumeDensity
          ? kVoxelGeometryProgramDomainVolume : 0u;
  return result;
}

VoxelGeometryProgram volumeDensityVoxelGeometryProgram(
    const VoxelGeometryFixed densityThreshold)
{
  VoxelGeometryProgram result{};
  result.domain = VoxelGeometryDomain::Volume;
  result.executionHint = VoxelGeometryExecutionHint::Cached;
  result.instructions = {
    {VoxelGeometryOpcode::LoadWorldNoise, 0u, 0u, 0u, 0x6d2b79f5},
    {VoxelGeometryOpcode::LoadConstant, 1u, 0u, 0u,
     std::clamp(densityThreshold, 0, kVoxelGeometryFixedOne)},
    {VoxelGeometryOpcode::Subtract, 0u, 0u, 1u},
    {VoxelGeometryOpcode::Clamp01, 0u, 0u},
    {VoxelGeometryOpcode::Quantize, 0u, 0u, 0u,
     kVoxelGeometryFixedOne},
    {VoxelGeometryOpcode::OutputCoverage, 0u, 0u},
  };
  return result;
}

} // namespace voxel::material
