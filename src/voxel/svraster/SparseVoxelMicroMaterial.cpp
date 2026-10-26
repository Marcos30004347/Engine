#include "voxel/svraster/SparseVoxelMicroMaterial.hpp"

namespace voxel::svraster
{
namespace
{

bool fail(std::string *error, const char *message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool byteValue(const uint32_t value)
{
  return value <= 0xffu;
}

} // namespace

bool packSparseVoxelMicroMaterial(const SparseVoxelMicroDescriptor &geometry, const SparseVoxelMicroShadingDescriptor &shading, SparseVoxelMicroMaterialGPU &packed, std::string *error)
{
  packed = {};
  if (error != nullptr)
    error->clear();
  if (!sparseVoxelMicroDescriptorIsValid(geometry))
    return fail(error, "Micro material geometry descriptor is invalid");
  if (static_cast<uint32_t>(shading.profile) > kSparseVoxelMicroShadingProfileMask || !byteValue(shading.variationStrength) || !byteValue(shading.parameter0) || !byteValue(shading.parameter1) ||
      !byteValue(shading.lodHandoff))
    return fail(error, "Micro material shading descriptor is invalid");

  // A disabled geometry record is canonical zero. This guarantees material
  // ID zero and absent extensions converge on the same no-load fast path.
  if (!sparseVoxelMicroDescriptorIsEnabled(geometry))
    return true;

  const bool staircase = geometry.geometryProfile ==
      SparseVoxelMicroGeometryProfile::Staircase;
  const uint32_t fixedDepth = staircase
      ? sparseVoxelStaircaseCarveDepthFixed(geometry.subdivision)
      : geometry.maximumCarveDepth * kSparseVoxelMicroDepthFractionScale +
          geometry.carveDepthFraction;
  const uint32_t faceMask = staircase
      ? kSparseVoxelStaircaseFaceMask
      : geometry.faceMask;
  packed.geometry = (geometry.subdivision & kSparseVoxelMicroSubdivisionMask) | ((fixedDepth & kSparseVoxelMicroMaximumDepthMask) << kSparseVoxelMicroMaximumDepthShift) |
                    ((faceMask & kSparseVoxelMicroFaceMask) << kSparseVoxelMicroFaceMaskShift) |
                    ((static_cast<uint32_t>(geometry.geometryProfile) & kSparseVoxelMicroGeometryProfileMask) << kSparseVoxelMicroGeometryProfileShift) |
                    ((static_cast<uint32_t>(shading.profile) & kSparseVoxelMicroShadingProfileMask) << kSparseVoxelMicroShadingProfileShift) | ((geometry.materialSeed & 0xffu) << kSparseVoxelMicroMaterialSeedShift);
  packed.shading = (shading.variationStrength & 0xffu) | ((shading.parameter0 & 0xffu) << 8u) | ((shading.parameter1 & 0xffu) << 16u) | ((shading.lodHandoff & 0xffu) << 24u);
  return true;
}

SparseVoxelMicroDescriptor unpackSparseVoxelMicroGeometry(const SparseVoxelMicroMaterialGPU &material)
{
  SparseVoxelMicroDescriptor descriptor{};
  descriptor.subdivision = material.geometry & kSparseVoxelMicroSubdivisionMask;
  const uint32_t fixedDepth =
      (material.geometry >> kSparseVoxelMicroMaximumDepthShift) &
      kSparseVoxelMicroMaximumDepthMask;
  descriptor.maximumCarveDepth =
      fixedDepth >> kSparseVoxelMicroDepthFractionBits;
  descriptor.carveDepthFraction =
      fixedDepth & (kSparseVoxelMicroDepthFractionScale - 1u);
  descriptor.faceMask = (material.geometry >> kSparseVoxelMicroFaceMaskShift) & kSparseVoxelMicroFaceMask;
  descriptor.geometryProfile = static_cast<SparseVoxelMicroGeometryProfile>((material.geometry >> kSparseVoxelMicroGeometryProfileShift) & kSparseVoxelMicroGeometryProfileMask);
  descriptor.materialSeed = material.geometry >> kSparseVoxelMicroMaterialSeedShift;
  return descriptor;
}

SparseVoxelMicroShadingDescriptor unpackSparseVoxelMicroShading(const SparseVoxelMicroMaterialGPU &material)
{
  SparseVoxelMicroShadingDescriptor descriptor{};
  descriptor.profile = static_cast<SparseVoxelMicroShadingProfile>((material.geometry >> kSparseVoxelMicroShadingProfileShift) & kSparseVoxelMicroShadingProfileMask);
  descriptor.variationStrength = material.shading & 0xffu;
  descriptor.parameter0 = (material.shading >> 8u) & 0xffu;
  descriptor.parameter1 = (material.shading >> 16u) & 0xffu;
  descriptor.lodHandoff = material.shading >> 24u;
  return descriptor;
}

bool sparseVoxelMicroMaterialIsValid(const SparseVoxelMicroMaterialGPU &material)
{
  if (material.geometry == 0u)
    return material.shading == 0u;
  const SparseVoxelMicroDescriptor geometry = unpackSparseVoxelMicroGeometry(material);
  return sparseVoxelMicroDescriptorIsEnabled(geometry) && static_cast<uint32_t>(unpackSparseVoxelMicroShading(material).profile) <= static_cast<uint32_t>(SparseVoxelMicroShadingProfile::FoliageTint);
}

SparseVoxelMicroMaterialLibrary::SparseVoxelMicroMaterialLibrary() : descriptors_(1u)
{
  deduplicated_.emplace(std::pair<uint32_t, uint32_t>{0u, 0u}, 0u);
}

bool SparseVoxelMicroMaterialLibrary::setMaterial(const uint32_t materialId, const SparseVoxelMicroMaterialGPU &descriptor, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!sparseVoxelMicroMaterialIsValid(descriptor))
    return fail(error, "Micro material packed descriptor is invalid");
  const std::pair<uint32_t, uint32_t> key{descriptor.geometry, descriptor.shading};
  auto found = deduplicated_.find(key);
  uint16_t descriptorId = 0u;
  if (found != deduplicated_.end())
  {
    descriptorId = found->second;
  }
  else
  {
    if (descriptors_.size() > 0xffffu)
      return fail(error, "Micro material descriptor library is full");
    descriptorId = static_cast<uint16_t>(descriptors_.size());
    descriptors_.push_back(descriptor);
    deduplicated_.emplace(key, descriptorId);
  }
  if (materialDescriptorIds_.size() <= materialId)
    materialDescriptorIds_.resize(static_cast<size_t>(materialId) + 1u, 0u);
  materialDescriptorIds_[materialId] = descriptorId;
  return true;
}

bool SparseVoxelMicroMaterialLibrary::setMaterial(const uint32_t materialId, const SparseVoxelMicroDescriptor &geometry, const SparseVoxelMicroShadingDescriptor &shading, std::string *error)
{
  SparseVoxelMicroMaterialGPU packed{};
  return packSparseVoxelMicroMaterial(geometry, shading, packed, error) && setMaterial(materialId, packed, error);
}

uint16_t SparseVoxelMicroMaterialLibrary::descriptorId(const uint32_t materialId) const
{
  return materialId < materialDescriptorIds_.size() ? materialDescriptorIds_[materialId] : 0u;
}

const SparseVoxelMicroMaterialGPU &SparseVoxelMicroMaterialLibrary::descriptor(const uint16_t descriptorId) const
{
  return descriptors_[descriptorId < descriptors_.size() ? descriptorId : 0u];
}

} // namespace voxel::svraster
