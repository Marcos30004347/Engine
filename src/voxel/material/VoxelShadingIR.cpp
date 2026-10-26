#include "voxel/material/VoxelShadingIR.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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
    *error = "Shading node " + std::to_string(index) +
        " violates constraint: " + constraint;
  return false;
}

bool writesRegister(const VoxelShadingOpcode opcode)
{
  return opcode != VoxelShadingOpcode::OutputLayerWeight &&
      opcode != VoxelShadingOpcode::OutputHueVariation &&
      opcode != VoxelShadingOpcode::OutputBrightness &&
      opcode != VoxelShadingOpcode::OutputRoughness &&
      opcode != VoxelShadingOpcode::OutputMetallic &&
      opcode != VoxelShadingOpcode::OutputPaletteWeight2 &&
      opcode != VoxelShadingOpcode::OutputPaletteWeight3;
}

bool isOutput(const VoxelShadingOpcode opcode)
{
  return opcode == VoxelShadingOpcode::OutputLayerWeight ||
      opcode == VoxelShadingOpcode::OutputHueVariation ||
      opcode == VoxelShadingOpcode::OutputBrightness ||
      opcode == VoxelShadingOpcode::OutputRoughness ||
      opcode == VoxelShadingOpcode::OutputMetallic ||
      opcode == VoxelShadingOpcode::OutputPaletteWeight2 ||
      opcode == VoxelShadingOpcode::OutputPaletteWeight3;
}

bool readsSource0(const VoxelShadingOpcode opcode)
{
  return opcode == VoxelShadingOpcode::Add ||
      opcode == VoxelShadingOpcode::Multiply ||
      opcode == VoxelShadingOpcode::OneMinus ||
      opcode == VoxelShadingOpcode::Clamp01 ||
      opcode == VoxelShadingOpcode::Smoothstep ||
      opcode == VoxelShadingOpcode::OutputLayerWeight ||
      opcode == VoxelShadingOpcode::OutputHueVariation ||
      opcode == VoxelShadingOpcode::OutputBrightness ||
      opcode == VoxelShadingOpcode::OutputRoughness ||
      opcode == VoxelShadingOpcode::OutputMetallic ||
      opcode == VoxelShadingOpcode::OutputPaletteWeight2 ||
      opcode == VoxelShadingOpcode::OutputPaletteWeight3 ||
      opcode == VoxelShadingOpcode::ScaleBias ||
      opcode == VoxelShadingOpcode::Maximum ||
      opcode == VoxelShadingOpcode::GreaterThan;
}

bool readsSource1(const VoxelShadingOpcode opcode)
{
  return opcode == VoxelShadingOpcode::Add ||
      opcode == VoxelShadingOpcode::Multiply ||
      opcode == VoxelShadingOpcode::Maximum;
}

float smoothstep(const float edge0, const float edge1, const float value)
{
  const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

} // namespace

bool VoxelShadingInstruction::operator==(
    const VoxelShadingInstruction &other) const
{
  return std::tie(opcode, destination, source0, source1, immediate0, immediate1) ==
      std::tie(other.opcode, other.destination, other.source0, other.source1,
               other.immediate0, other.immediate1);
}

bool VoxelShadingInstruction::operator<(
    const VoxelShadingInstruction &other) const
{
  return std::tie(opcode, destination, source0, source1, immediate0, immediate1) <
      std::tie(other.opcode, other.destination, other.source0, other.source1,
               other.immediate0, other.immediate1);
}

bool verifyVoxelShadingProgram(
    const VoxelShadingProgram &program, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (program.instructions.empty())
    return true;
  if (program.instructions.size() > kMaximumVoxelShadingInstructionCount)
    return fail(error, "Voxel shading program exceeds the instruction limit");

  std::array<bool, kVoxelShadingRegisterCount> initialized{};
  uint32_t layerOutputCount = 0u;
  uint32_t hueOutputCount = 0u;
  uint32_t brightnessOutputCount = 0u;
  uint32_t roughnessOutputCount = 0u;
  uint32_t metallicOutputCount = 0u;
  std::array<uint32_t, 2> additionalPaletteOutputCount{};
  bool outputPhase = false;
  for (size_t index = 0u; index < program.instructions.size(); ++index)
  {
    const VoxelShadingInstruction &instruction = program.instructions[index];
    if (static_cast<uint32_t>(instruction.opcode) >
        static_cast<uint32_t>(VoxelShadingOpcode::OutputPaletteWeight3))
      return failNode(error, index, "unsupported opcode");
    if (!std::isfinite(instruction.immediate0) ||
        !std::isfinite(instruction.immediate1))
      return failNode(error, index, "constant is non-finite");
    if (writesRegister(instruction.opcode) &&
        instruction.destination >= kVoxelShadingRegisterCount)
      return failNode(error, index, "destination register is out of range");
    if (instruction.opcode == VoxelShadingOpcode::LoadParameter &&
        instruction.source0 >= 4u)
      return failNode(error, index, "parameter index is out of range");
    if (instruction.opcode == VoxelShadingOpcode::LoadField &&
        instruction.source0 >= kVoxelMaterialFieldCount)
      return failNode(error, index, "material field index is out of range");
    if (readsSource0(instruction.opcode) &&
        (instruction.source0 >= kVoxelShadingRegisterCount ||
         !initialized[instruction.source0]))
      return failNode(error, index, "source register is uninitialized");
    if (readsSource1(instruction.opcode) &&
        (instruction.source1 >= kVoxelShadingRegisterCount ||
         !initialized[instruction.source1]))
      return failNode(error, index, "second source register is uninitialized");
    if (instruction.opcode == VoxelShadingOpcode::Smoothstep &&
        instruction.immediate0 >= instruction.immediate1)
      return failNode(error, index, "smoothstep edges are not increasing");
    if (isOutput(instruction.opcode))
    {
      outputPhase = true;
      if (instruction.opcode == VoxelShadingOpcode::OutputLayerWeight)
        ++layerOutputCount;
      else if (instruction.opcode == VoxelShadingOpcode::OutputHueVariation)
        ++hueOutputCount;
      else if (instruction.opcode == VoxelShadingOpcode::OutputBrightness)
        ++brightnessOutputCount;
      else if (instruction.opcode == VoxelShadingOpcode::OutputRoughness)
        ++roughnessOutputCount;
      else if (instruction.opcode == VoxelShadingOpcode::OutputMetallic)
        ++metallicOutputCount;
      else
        ++additionalPaletteOutputCount[
            instruction.opcode == VoxelShadingOpcode::OutputPaletteWeight2
                ? 0u : 1u];
    }
    else
    {
      if (outputPhase)
        return failNode(error, index, "calculation follows an output node");
      initialized[instruction.destination] = true;
    }
  }
  if (layerOutputCount != 1u)
    return fail(error, "Voxel shading program must contain one layer-weight output");
  if (hueOutputCount > 1u || brightnessOutputCount > 1u ||
      roughnessOutputCount > 1u || metallicOutputCount > 1u)
    return fail(error, "Voxel shading program contains a duplicate colour output");
  if (additionalPaletteOutputCount[0] > 1u ||
      additionalPaletteOutputCount[1] > 1u)
    return fail(error, "Voxel shading program contains a duplicate palette output");
  const uint32_t materialFields = voxelShadingRequiredMaterialFields(program);
  uint32_t materialFieldCount = 0u;
  for (uint32_t mask = materialFields; mask != 0u; mask >>= 1u)
    materialFieldCount += mask & 1u;
  if (materialFieldCount > kMaximumVoxelShadingMaterialFieldCount)
    return fail(error, "Voxel shading program exceeds the material field limit");
  return true;
}

bool voxelShadingProgramUsesTerrainSlope(const VoxelShadingProgram &program)
{
  return std::any_of(
      program.instructions.begin(), program.instructions.end(),
      [](const VoxelShadingInstruction &instruction)
      {
        return instruction.opcode == VoxelShadingOpcode::LoadTerrainSlope ||
            instruction.opcode ==
                VoxelShadingOpcode::LoadTerrainSlopeClassified;
      });
}

uint32_t voxelShadingRequiredMaterialFields(
    const VoxelShadingProgram &program)
{
  uint32_t result = 0u;
  for (const VoxelShadingInstruction &instruction : program.instructions)
    if (instruction.opcode == VoxelShadingOpcode::LoadField &&
        instruction.source0 < kVoxelMaterialFieldCount)
      result |= 1u << instruction.source0;
  return result;
}

uint32_t voxelShadingAdditionalPaletteMask(
    const VoxelShadingProgram &program)
{
  uint32_t result = 0u;
  for (const VoxelShadingInstruction &instruction : program.instructions)
  {
    if (instruction.opcode == VoxelShadingOpcode::OutputPaletteWeight2)
      result |= 1u;
    else if (instruction.opcode == VoxelShadingOpcode::OutputPaletteWeight3)
      result |= 2u;
  }
  return result;
}

bool compileVoxelShadingProgram(
    const VoxelShadingProgram &program,
    std::vector<VoxelShadingInstructionGPU> &instructions,
    std::string *error)
{
  instructions.clear();
  if (!verifyVoxelShadingProgram(program, error))
    return false;
  instructions.reserve(program.instructions.size());
  for (const VoxelShadingInstruction &instruction : program.instructions)
  {
    VoxelShadingInstructionGPU packed{};
    packed.opcodeOperands =
        (static_cast<uint32_t>(instruction.opcode) & 0xffu) |
        ((instruction.destination & 0xffu) << 8u) |
        ((instruction.source0 & 0xffu) << 16u) |
        ((instruction.source1 & 0xffu) << 24u);
    packed.immediate0 = instruction.immediate0;
    packed.immediate1 = instruction.immediate1;
    instructions.push_back(packed);
  }
  return true;
}

bool evaluateVoxelShadingProgram(
    const VoxelShadingProgram &program, const VoxelShadingInputs &inputs,
    VoxelShadingResult &result, std::string *error)
{
  result = {};
  if (!verifyVoxelShadingProgram(program, error) || program.instructions.empty())
    return program.instructions.empty();

  std::array<float, kVoxelShadingRegisterCount> registers{};
  for (const VoxelShadingInstruction &instruction : program.instructions)
  {
    switch (instruction.opcode)
    {
    case VoxelShadingOpcode::LoadNormalizedDepth:
      registers[instruction.destination] = inputs.normalizedDepth;
      break;
    case VoxelShadingOpcode::LoadStableRandom:
      registers[instruction.destination] = inputs.stableRandom;
      break;
    case VoxelShadingOpcode::LoadTerrainSlope:
      registers[instruction.destination] =
          std::clamp(inputs.terrainSlope, 0.0f, 1.0f);
      break;
    case VoxelShadingOpcode::LoadConstant:
      registers[instruction.destination] = instruction.immediate0;
      break;
    case VoxelShadingOpcode::Add:
      registers[instruction.destination] =
          registers[instruction.source0] + registers[instruction.source1];
      break;
    case VoxelShadingOpcode::Multiply:
      registers[instruction.destination] =
          registers[instruction.source0] * registers[instruction.source1];
      break;
    case VoxelShadingOpcode::OneMinus:
      registers[instruction.destination] = 1.0f - registers[instruction.source0];
      break;
    case VoxelShadingOpcode::Clamp01:
      registers[instruction.destination] =
          std::clamp(registers[instruction.source0], 0.0f, 1.0f);
      break;
    case VoxelShadingOpcode::Smoothstep:
      registers[instruction.destination] = smoothstep(
          instruction.immediate0, instruction.immediate1,
          registers[instruction.source0]);
      break;
    case VoxelShadingOpcode::OutputLayerWeight:
      result.layerWeight =
          std::clamp(registers[instruction.source0], 0.0f, 1.0f);
      break;
    case VoxelShadingOpcode::OutputHueVariation:
      result.hueVariation =
          std::clamp(registers[instruction.source0], -1.0f, 1.0f);
      break;
    case VoxelShadingOpcode::OutputBrightness:
      result.brightness =
          std::clamp(registers[instruction.source0], 0.0f, 2.0f);
      break;
    case VoxelShadingOpcode::OutputRoughness:
      result.roughness =
          std::clamp(registers[instruction.source0], 0.0f, 1.0f);
      break;
    case VoxelShadingOpcode::OutputMetallic:
      result.metallic =
          std::clamp(registers[instruction.source0], 0.0f, 1.0f);
      break;
    case VoxelShadingOpcode::ScaleBias:
      registers[instruction.destination] =
          registers[instruction.source0] * instruction.immediate0 +
          instruction.immediate1;
      break;
    case VoxelShadingOpcode::LoadSideOrRiser:
      registers[instruction.destination] = inputs.sideOrRiser ? 1.0f : 0.0f;
      break;
    case VoxelShadingOpcode::Maximum:
      registers[instruction.destination] = std::max(
          registers[instruction.source0], registers[instruction.source1]);
      break;
    case VoxelShadingOpcode::GreaterThan:
      registers[instruction.destination] =
          registers[instruction.source0] > instruction.immediate0 ? 1.0f : 0.0f;
      break;
    case VoxelShadingOpcode::LoadTerrainSlopeClassified:
      registers[instruction.destination] =
          inputs.terrainSlopeClassified ? 1.0f : 0.0f;
      break;
    case VoxelShadingOpcode::LoadParameter:
      registers[instruction.destination] =
          inputs.parameters.values[instruction.source0];
      break;
    case VoxelShadingOpcode::LoadField:
      registers[instruction.destination] =
          std::clamp(inputs.fields[instruction.source0], 0.0f, 1.0f);
      break;
    case VoxelShadingOpcode::OutputPaletteWeight2:
      result.paletteWeights[2] =
          std::clamp(registers[instruction.source0], 0.0f, 1.0f);
      break;
    case VoxelShadingOpcode::OutputPaletteWeight3:
      result.paletteWeights[3] =
          std::clamp(registers[instruction.source0], 0.0f, 1.0f);
      break;
    }
  }
  result.paletteWeights[1] = result.layerWeight;
  const float nonPrimary = result.paletteWeights[1] +
      result.paletteWeights[2] + result.paletteWeights[3];
  if (nonPrimary > 1.0f)
    for (uint32_t index = 1u; index < kVoxelShadingPaletteColourCount; ++index)
      result.paletteWeights[index] /= nonPrimary;
  result.paletteWeights[0] = std::max(1.0f - nonPrimary, 0.0f);
  result.layerWeight = result.paletteWeights[1];
  return true;
}

std::array<float, 3> blendVoxelShadingPalette(
    const std::array<std::array<float, 3>, kVoxelShadingPaletteColourCount>
        &colours,
    const VoxelShadingResult &result)
{
  std::array<float, 3> blended{};
  for (uint32_t layer = 0u; layer < kVoxelShadingPaletteColourCount; ++layer)
    for (uint32_t channel = 0u; channel < blended.size(); ++channel)
      blended[channel] += colours[layer][channel] *
          result.paletteWeights[layer];
  return blended;
}

bool evaluateVoxelShadingProgram(
    const VoxelShadingProgram &program, const VoxelShadingInputs &inputs,
    float &layerWeight, std::string *error)
{
  VoxelShadingResult result{};
  if (!evaluateVoxelShadingProgram(program, inputs, result, error))
    return false;
  layerWeight = result.layerWeight;
  return true;
}

} // namespace voxel::material
