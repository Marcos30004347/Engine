#include <array>
#include <cassert>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>

#include "material/MaterialGBufferOutputs.hpp"
#include "virtualgeometry/Material.hpp"
#include "virtualgeometry/VirtualMaterialFile.hpp"

namespace
{

bool nearEqual(float a, float b, float tolerance = 1.0e-6f)
{
  return std::abs(a - b) <= tolerance;
}

void testMaterialGBufferOutputLayout()
{
  using material::MaterialGBufferOutputGPU;

  static_assert(sizeof(MaterialGBufferOutputGPU) == 64u);
  static_assert(offsetof(MaterialGBufferOutputGPU, baseColorOpacity) == 0u);
  static_assert(offsetof(MaterialGBufferOutputGPU, normalRoughness) == 16u);
  static_assert(offsetof(MaterialGBufferOutputGPU, emissiveMetallic) == 32u);
  static_assert(offsetof(MaterialGBufferOutputGPU, flags) == 48u);

  const MaterialGBufferOutputGPU output{};
  assert(nearEqual(output.baseColorOpacity[0], 0.85f));
  assert(nearEqual(output.baseColorOpacity[1], 0.85f));
  assert(nearEqual(output.baseColorOpacity[2], 0.85f));
  assert(nearEqual(output.baseColorOpacity[3], 1.0f));
  assert(nearEqual(output.normalRoughness[2], 1.0f));
  assert(nearEqual(output.normalRoughness[3], 1.0f));
  assert(nearEqual(output.emissiveMetallic[3], 0.0f));
  assert(output.flags[0] == material::MaterialGBufferFlag_None);

  std::cout << "  [OK] Material GBuffer output layout\n";
}

void testMaterialGBufferOutputConstruction()
{
  const auto output = material::makeMaterialGBufferOutput(
      {0.2f, 0.4f, 0.8f, 0.5f},
      {0.0f, 1.0f, 0.0f},
      0.35f,
      0.75f,
      {1.0f, 2.0f, 3.0f},
      material::MaterialGBufferFlag_HasBaseColorTexture | material::MaterialGBufferFlag_HasNormalTexture);

  assert(nearEqual(output.baseColorOpacity[0], 0.2f));
  assert(nearEqual(output.baseColorOpacity[3], 0.5f));
  assert(nearEqual(output.normalRoughness[1], 1.0f));
  assert(nearEqual(output.normalRoughness[3], 0.35f));
  assert(nearEqual(output.emissiveMetallic[0], 1.0f));
  assert(nearEqual(output.emissiveMetallic[2], 3.0f));
  assert(nearEqual(output.emissiveMetallic[3], 0.75f));
  assert((output.flags[0] & material::MaterialGBufferFlag_HasBaseColorTexture) != 0u);
  assert((output.flags[0] & material::MaterialGBufferFlag_HasNormalTexture) != 0u);

  std::cout << "  [OK] Material GBuffer output construction\n";
}

void testDirectOnlyParityMath()
{
  const auto output = material::makeMaterialGBufferOutput(
      {0.8f, 0.25f, 0.5f, 0.9f},
      {0.0f, 0.0f, 1.0f},
      1.0f,
      0.0f,
      {0.0f, 0.0f, 0.0f},
      material::MaterialGBufferFlag_None);

  const std::array<float, 3> directLighting{0.5f, 0.25f, 2.0f};
  const auto shaded = material::evaluateDirectOnlyMaterialColor(output, directLighting);

  assert(nearEqual(shaded[0], 0.8f * 0.5f));
  assert(nearEqual(shaded[1], 0.25f * 0.25f));
  assert(nearEqual(shaded[2], 0.5f * 2.0f));
  assert(nearEqual(shaded[3], 0.9f));

  std::cout << "  [OK] Direct-only material parity math\n";
}

void testDirectOnlyEmissiveTerm()
{
  const auto output = material::makeMaterialGBufferOutput(
      {0.1f, 0.2f, 0.3f, 1.0f},
      {0.0f, 0.0f, 1.0f},
      1.0f,
      0.0f,
      {2.0f, 3.0f, 4.0f},
      material::MaterialGBufferFlag_None);

  const auto shaded = material::evaluateDirectOnlyMaterialColor(output, {1.0f, 1.0f, 1.0f});
  assert(nearEqual(shaded[0], 2.1f));
  assert(nearEqual(shaded[1], 3.2f));
  assert(nearEqual(shaded[2], 4.3f));
  assert(nearEqual(shaded[3], 1.0f));

  std::cout << "  [OK] Direct-only emissive term\n";
}

void testVirtualMaterialPBRRoundTrip()
{
  virtualgeometry::Material::Definition definition{};
  definition.shaderPath = virtualgeometry::Material::defaultShaderPath();
  definition.baseColorFactor[0] = 0.25f;
  definition.baseColorFactor[1] = 0.5f;
  definition.baseColorFactor[2] = 0.75f;
  definition.baseColorFactor[3] = 0.8f;
  definition.roughnessFactor = 0.35f;
  definition.metallicFactor = 0.65f;
  definition.emissiveFactor[0] = 1.0f;
  definition.emissiveFactor[1] = 0.5f;
  definition.emissiveFactor[2] = 0.25f;
  definition.emissiveStrength = 4.0f;
  definition.opacity = 0.8f;

  const std::filesystem::path path = std::filesystem::temp_directory_path() / "engine_material_phase1_roundtrip.vmat";
  assert(virtualgeometry::VirtualMaterialFile::save(path.string(), definition));

  virtualgeometry::Material loaded;
  assert(virtualgeometry::VirtualMaterialFile::load(path.string(), loaded));

  const float *baseColorFactor = loaded.getBaseColorFactor();
  assert(nearEqual(baseColorFactor[0], 0.25f));
  assert(nearEqual(baseColorFactor[1], 0.5f));
  assert(nearEqual(baseColorFactor[2], 0.75f));
  assert(nearEqual(baseColorFactor[3], 0.8f));
  assert(nearEqual(loaded.getRoughnessFactor(), 0.35f));
  assert(nearEqual(loaded.getMetallicFactor(), 0.65f));
  const float *emissiveFactor = loaded.getEmissiveFactor();
  assert(nearEqual(emissiveFactor[0], 1.0f));
  assert(nearEqual(emissiveFactor[1], 0.5f));
  assert(nearEqual(emissiveFactor[2], 0.25f));
  assert(nearEqual(loaded.getEmissiveStrength(), 4.0f));
  assert(nearEqual(loaded.getOpacity(), 0.8f));

  std::filesystem::remove(path);
  std::cout << "  [OK] Virtual material PBR roundtrip\n";
}

} // namespace

int main()
{
  testMaterialGBufferOutputLayout();
  testMaterialGBufferOutputConstruction();
  testDirectOnlyParityMath();
  testDirectOnlyEmissiveTerm();
  testVirtualMaterialPBRRoundTrip();

  std::cout << "MaterialPhase1Tests passed" << std::endl;
  return 0;
}
