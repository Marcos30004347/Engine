#include "voxel/material/VoxelMaterialContext.hpp"

#include <algorithm>
#include <limits>

namespace voxel::material
{

void VoxelAuxContextPool::begin(const uint32_t voxelCount)
{
  expectedVoxelCount_ = voxelCount;
  indices_.clear();
  indices_.reserve(voxelCount);
  values_.assign(1u, 0u);
}

bool VoxelAuxContextPool::append(
    const VoxelAuxContext &contextValue, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (indices_.size() >= expectedVoxelCount_)
  {
    if (error != nullptr)
      *error = "Voxel auxiliary context has more entries than page voxels";
    return false;
  }
  const uint32_t packed = packVoxelAuxContext(contextValue);
  auto found = std::find(values_.begin(), values_.end(), packed);
  if (found == values_.end())
  {
    if (values_.size() > std::numeric_limits<uint16_t>::max())
    {
      if (error != nullptr)
        *error = "Voxel auxiliary context pool exceeds 16-bit indexing";
      return false;
    }
    values_.push_back(packed);
    found = values_.end() - 1;
  }
  indices_.push_back(static_cast<uint16_t>(found - values_.begin()));
  return true;
}

VoxelAuxContext VoxelAuxContextPool::context(const uint32_t voxelIndex) const
{
  if (voxelIndex >= indices_.size() || indices_[voxelIndex] >= values_.size())
    return {};
  return unpackVoxelAuxContext(values_[indices_[voxelIndex]]);
}

VoxelAuxContextMemory VoxelAuxContextPool::memory() const
{
  if (!enabled())
    return {};
  return {
    indices_.size(), values_.size(),
    indices_.size() * sizeof(uint16_t) + values_.size() * sizeof(uint32_t),
    indices_.size() * sizeof(uint32_t),
  };
}

bool voxelPageRequiresContextRebuild(
    const VoxelPageContextState &page, const uint16_t changedMaterial,
    const uint32_t previousFields, const uint32_t nextFields)
{
  const uint32_t newlyRequired = nextFields & ~previousFields;
  if ((newlyRequired & ~page.bakedFields) == 0u)
    return false;
  return std::find(
      page.materialHandles.begin(), page.materialHandles.end(),
      changedMaterial) != page.materialHandles.end();
}

std::vector<uint64_t> voxelPagesRequiringContextRebuild(
    const std::vector<VoxelPageContextState> &pages,
    const uint16_t changedMaterial, const uint32_t previousFields,
    const uint32_t nextFields)
{
  std::vector<uint64_t> result;
  for (const VoxelPageContextState &page : pages)
    if (voxelPageRequiresContextRebuild(
            page, changedMaterial, previousFields, nextFields))
      result.push_back(page.pageId);
  return result;
}

std::string voxelMaterialContextFieldNames(uint32_t fields)
{
  struct FieldName
  {
    VoxelMaterialContextField field;
    const char *name;
  };
  constexpr FieldName names[] = {
    {VoxelMaterialContextField::BaseSurface, "base surface"},
    {VoxelMaterialContextField::MicroSlope, "micro slope"},
    {VoxelMaterialContextField::TerrainSlope, "terrain slope"},
    {VoxelMaterialContextField::DepthBelowSurface, "depth below surface"},
    {VoxelMaterialContextField::TerrainBlend, "terrain blend"},
    {VoxelMaterialContextField::Auxiliary, "auxiliary"},
  };
  std::string result;
  for (const FieldName &entry : names)
  {
    const uint32_t bit = static_cast<uint32_t>(entry.field);
    if ((fields & bit) == 0u)
      continue;
    if (!result.empty())
      result += ", ";
    result += entry.name;
    fields &= ~bit;
  }
  if (fields != 0u)
  {
    if (!result.empty())
      result += ", ";
    result += "unknown context";
  }
  return result.empty() ? "none" : result;
}

} // namespace voxel::material
