#pragma once

#include "voxel/svraster/CityVoxelSceneFormat.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace virtualgeometry {
// Offline release gate. The expected owners must come from the authored city,
// not from the package being checked.
struct CityVoxelExpectedOwner
{
  uint64_t id = 0;
  std::string category;
};

struct CityVoxelReleaseAuditReport
{
  uint64_t instances = 0, owners = 0, geometry = 0, appearances = 0, occupiedCells = 0;
  uint64_t checkedPages = 0, fallbackCells = 0, traceAssets = 0, vgAssets = 0;
  uint64_t missingOwners = 0, unexpectedOwners = 0, unknownMaterialProfiles = 0;
  uint64_t nonOpaqueVoxels = 0;
  std::map<std::string, uint64_t> expectedByCategory, coveredByCategory, missingByCategory;
  std::vector<uint64_t> missingOwnerIds, unexpectedOwnerIds;
};

bool auditCityVoxelSceneForRelease(
    const std::string &path,
    const std::vector<CityVoxelExpectedOwner> &expectedOwners,
    const voxel::svraster::CityVoxelSceneLimits &limits,
    uint32_t maximumDecodedCellsPerAsset,
    CityVoxelReleaseAuditReport &report,
    std::string *error = nullptr);
} // namespace virtualgeometry
