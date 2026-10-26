#pragma once

#include <algorithm>
#include <cstdint>

#include "globalIllumination/surfels/SurfelPersistenceTypes.hpp"

namespace globalillumination
{
namespace surfels
{

enum class SurfelHierarchyDebugMode : uint32_t
{
  Region = 0u,
  CellId = 1u,
  Frustum = 2u,
  Slice = 3u,
};

struct SurfelHierarchySettings
{
  uint32_t viewPortWidth = 1920u;
  uint32_t viewPortHeight = 1080u;
  uint32_t lookupCellResolution = 16u;
  float centralGridHalfExtent = 0.2f;
  float gridScale = 10.0f;
  float frustumFarDistance = 4096.0f;
  uint32_t gridCapacity = surfelLookupTotalCellCount(16u);
};

inline float surfelHierarchyBaseCellSize(const SurfelHierarchySettings &settings)
{
  const float halfExtent = std::max(settings.centralGridHalfExtent, 1.0e-4f);
  return (halfExtent * 2.0f) / static_cast<float>(std::max(1u, settings.lookupCellResolution));
}

inline float surfelHierarchyCentralHalfExtent(const SurfelHierarchySettings &settings)
{
  return std::max(settings.centralGridHalfExtent, 1.0e-4f);
}

inline float surfelHierarchyGridScaleFromHalfExtent(const SurfelHierarchySettings &settings)
{
  return 1.0f / (std::max(surfelHierarchyBaseCellSize(settings), 1.0e-4f) * 4.0f);
}

inline uint32_t surfelHierarchyLookupCellCount(const SurfelHierarchySettings &settings)
{
  return surfelLookupTotalCellCount(std::max(1u, settings.lookupCellResolution));
}

} // namespace surfels
} // namespace globalillumination
