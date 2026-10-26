#pragma once

#include <array>
#include <cstdint>

namespace material
{

enum MaterialGBufferFlags : uint32_t
{
  MaterialGBufferFlag_None = 0u,
  MaterialGBufferFlag_HasBaseColorTexture = 1u << 0u,
  MaterialGBufferFlag_HasNormalTexture = 1u << 1u,
  MaterialGBufferFlag_HasEmissiveTexture = 1u << 2u,
  MaterialGBufferFlag_AlphaMasked = 1u << 3u,
};

struct MaterialGBufferOutputGPU
{
  // rgb = base color, a = opacity
  std::array<float, 4> baseColorOpacity{0.85f, 0.85f, 0.85f, 1.0f};

  // xyz = shading normal, w = roughness
  std::array<float, 4> normalRoughness{0.0f, 0.0f, 1.0f, 1.0f};

  // rgb = emitted radiance, w = metallic
  std::array<float, 4> emissiveMetallic{0.0f, 0.0f, 0.0f, 0.0f};

  // x = flags, yzw reserved
  std::array<uint32_t, 4> flags{MaterialGBufferFlag_None, 0u, 0u, 0u};
};

inline MaterialGBufferOutputGPU makeDefaultMaterialGBufferOutput()
{
  return MaterialGBufferOutputGPU{};
}

inline MaterialGBufferOutputGPU makeMaterialGBufferOutput(
    const std::array<float, 4> &baseColorOpacity,
    const std::array<float, 3> &shadingNormal,
    float roughness,
    float metallic,
    const std::array<float, 3> &emissiveRadiance,
    uint32_t flags)
{
  MaterialGBufferOutputGPU output{};
  output.baseColorOpacity = baseColorOpacity;
  output.normalRoughness = {shadingNormal[0], shadingNormal[1], shadingNormal[2], roughness};
  output.emissiveMetallic = {emissiveRadiance[0], emissiveRadiance[1], emissiveRadiance[2], metallic};
  output.flags = {flags, 0u, 0u, 0u};
  return output;
}

inline std::array<float, 4> evaluateDirectOnlyMaterialColor(
    const MaterialGBufferOutputGPU &materialOutput,
    const std::array<float, 3> &directLighting)
{
  return {
    materialOutput.baseColorOpacity[0] * directLighting[0] + materialOutput.emissiveMetallic[0],
    materialOutput.baseColorOpacity[1] * directLighting[1] + materialOutput.emissiveMetallic[1],
    materialOutput.baseColorOpacity[2] * directLighting[2] + materialOutput.emissiveMetallic[2],
    materialOutput.baseColorOpacity[3],
  };
}

static_assert(sizeof(MaterialGBufferOutputGPU) == 64u);

} // namespace material
