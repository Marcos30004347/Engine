#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sdf/SDFSceneFile.hpp"
#include "editor/virtualgeometry/VirtualGeometryBuilder.hpp"

namespace virtualgeometry
{

struct SparseSDFBuildSettings
{
  float requestedError = 0.05f;
  uint32_t maxLevel = 1u;
  uint32_t narrowBandVoxels = 4u;
  uint32_t guardLeafLevels = 0u;
  uint32_t sdfWorkerCount = 64u;
  uint32_t maxRefinedCellsPerBrick = sdf::VSDF_BRICK_CELL_COUNT;
  uint32_t surfaceCloseVoxels = 1u;
  bool progressLogging = true;
  uint32_t progressLogBrickInterval = 16u;
};

class SparseSDFBuilder
{
public:
  static std::vector<sdf::VSDFInstanceWriteData> buildInstances(
      const VirtualGeometrySourceAsset &asset,
      const SparseSDFBuildSettings &settings = {});

  static bool writeVSDF(
      const std::string &path,
      const VirtualGeometrySourceAsset &asset,
      const SparseSDFBuildSettings &settings,
      std::string *error = nullptr);

  static uint64_t buildSettingsHash(const SparseSDFBuildSettings &settings);
};

} // namespace virtualgeometry
