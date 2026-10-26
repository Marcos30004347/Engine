#pragma once

#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/SparseVoxelMicroGeometry.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace voxel::svraster
{

enum class SparseVoxelMicroShadingProfile : uint32_t
{
  Linear = 0u,
  HardCrust = 1u,
  NoisyLinear = 2u,
  ExposedSides = 3u,
  FoliageTint = 4u,
};

struct SparseVoxelMicroShadingDescriptor
{
  SparseVoxelMicroShadingProfile profile = SparseVoxelMicroShadingProfile::Linear;
  uint32_t variationStrength = 0u;
  uint32_t parameter0 = 0u;
  uint32_t parameter1 = 0u;
  uint32_t lodHandoff = 0u;
};

bool packSparseVoxelMicroMaterial(const SparseVoxelMicroDescriptor &geometry, const SparseVoxelMicroShadingDescriptor &shading, SparseVoxelMicroMaterialGPU &packed, std::string *error = nullptr);

bool sparseVoxelMicroMaterialIsValid(const SparseVoxelMicroMaterialGPU &material);

SparseVoxelMicroDescriptor unpackSparseVoxelMicroGeometry(const SparseVoxelMicroMaterialGPU &material);

SparseVoxelMicroShadingDescriptor unpackSparseVoxelMicroShading(const SparseVoxelMicroMaterialGPU &material);

class SparseVoxelMicroMaterialLibrary
{
public:
  SparseVoxelMicroMaterialLibrary();

  // Assigning a material the all-zero descriptor clears its extension. Equal
  // descriptors share one stable 16-bit ID. IDs are never recycled during the
  // library lifetime, keeping already-built palettes valid. Per-voxel terrain
  // context is stored independently from this material identity.
  bool setMaterial(uint32_t materialId, const SparseVoxelMicroMaterialGPU &descriptor, std::string *error = nullptr);
  bool setMaterial(uint32_t materialId, const SparseVoxelMicroDescriptor &geometry, const SparseVoxelMicroShadingDescriptor &shading = {}, std::string *error = nullptr);

  uint16_t descriptorId(uint32_t materialId) const;
  const SparseVoxelMicroMaterialGPU &descriptor(uint16_t descriptorId) const;
  const std::vector<SparseVoxelMicroMaterialGPU> &descriptors() const
  {
    return descriptors_;
  }

private:
  std::vector<SparseVoxelMicroMaterialGPU> descriptors_;
  std::vector<uint16_t> materialDescriptorIds_;
  std::map<std::pair<uint32_t, uint32_t>, uint16_t> deduplicated_;
};

} // namespace voxel::svraster
