#include "editor/virtualgeometry/CityVoxelSparseSceneBridge.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace virtualgeometry {
namespace {
bool fail(std::string *error, const char *message)
{
  if (error)
  {
    *error = message;
  }
  return false;
}

bool cellCoordinate(double metres, double pitch, int32_t &out)
{
  const double value = metres / pitch;
  const double rounded = std::round(value);
  if (!std::isfinite(value) || std::abs(value - rounded) > 0.02 || rounded < std::numeric_limits<int32_t>::min() || rounded > std::numeric_limits<int32_t>::max())
  {
    return false;
  }
  out = static_cast<int32_t>(rounded);
  return true;
}

bool identity(const std::array<float, 16> &matrix)
{
  for (unsigned i = 0; i < matrix.size(); ++i)
  {
    if (std::abs(matrix[i] - ((i % 5u) == 0u ? 1.0f : 0.0f)) > 0.00001f)
    {
      return false;
    }
  }
  return true;
}
} // namespace

bool compileCityVoxelSparseScene(
    const voxel::svraster::SparseVoxelScene &source,
    voxel::svraster::CityVoxelAssetCompiler &compiler,
    uint32_t pitchMicrometres,
    uint64_t maximumExpandedCellsPerModel,
    uint64_t firstStableId,
    std::vector<voxel::svraster::CityVoxelCompiledPlacement> &placements,
    std::string *error,
    const CityVoxelOverlapCutouts *cutouts)
{
  using namespace voxel::svraster;
  if (error)
  {
    error->clear();
  }
  if (!pitchMicrometres || !maximumExpandedCellsPerModel || !firstStableId || source.models.empty() || source.instances.empty() || source.instances.size() > std::numeric_limits<uint64_t>::max() - firstStableId)
  {
    return fail(error, "City voxel import needs a pitch, cell cap, models and instances");
  }

  const double pitch = double(pitchMicrometres) * 1e-6;
  std::set<std::array<int32_t, 3>> cutterCells;
  if (cutouts)
  {
    if (cutouts->targetInstance >= source.instances.size() || cutouts->cutterInstances.empty())
      return fail(error, "Invalid overlap cutout target or empty cutter set");
    for (uint32_t cutter : cutouts->cutterInstances)
    {
      if (cutter >= source.instances.size() || cutter == cutouts->targetInstance)
        return fail(error, "Invalid overlap cutter index");
      const auto &instance = source.instances[cutter];
      if (instance.blasIndex >= source.models.size() || !identity(instance.modelToWorld))
        return fail(error, "Overlap cutters require world-space identity instances");
      const auto &model = source.models[instance.blasIndex];
      if (model.hierarchyNodes.size() != model.centersLengths.size())
        return fail(error, "Overlap cutter hierarchy is incomplete");
      for (size_t node = 0; node < model.hierarchyNodes.size(); ++node)
      {
        if (model.hierarchyNodes[node].childCount) continue;
        const auto &box = model.centersLengths[node];
        const double span = double(box.length) / pitch;
        const double rounded = std::round(span);
        if (!std::isfinite(span) || rounded < 1.0 || rounded > 1024.0 ||
            std::abs(span - rounded) > 0.02)
          return fail(error, "Overlap cutter leaf is off the voxel grid");
        const uint32_t side = static_cast<uint32_t>(rounded);
        if (side > maximumExpandedCellsPerModel / side ||
            uint64_t(side)*side > maximumExpandedCellsPerModel / side ||
            uint64_t(side)*side*side > maximumExpandedCellsPerModel)
          return fail(error, "Overlap cutter leaf exceeds the expanded cell cap");
        std::array<int32_t,3> minimum{};
        for (unsigned axis = 0; axis < 3; ++axis)
          if (!cellCoordinate(double(box.center[axis]) - double(box.length)*0.5, pitch,
                              minimum[axis]) ||
              int64_t(minimum[axis]) + side - 1 > INT32_MAX)
            return fail(error, "Overlap cutter exceeds the voxel grid");
        for (uint32_t z=0; z<side; ++z)
          for (uint32_t y=0; y<side; ++y)
            for (uint32_t x=0; x<side; ++x)
              cutterCells.insert({minimum[0]+int32_t(x), minimum[1]+int32_t(y),
                                  minimum[2]+int32_t(z)});
        if (cutterCells.size() > maximumExpandedCellsPerModel)
          return fail(error, "Overlap cutter set exceeds the expanded cell cap");
      }
    }
  }
  std::vector<CityVoxelCompiledPlacement> next;
  next.reserve(source.instances.size());
  for (size_t instanceIndex = 0; instanceIndex < source.instances.size(); ++instanceIndex)
  {
    const auto &instance = source.instances[instanceIndex];
    if (instance.blasIndex >= source.models.size() || !identity(instance.modelToWorld))
    {
      return fail(error, "City voxel OBJ bridge requires valid world-space identity instances");
    }
    const auto &model = source.models[instance.blasIndex];
    const size_t count = model.hierarchyNodes.size();
    if (!count || model.centersLengths.size() != count || model.materials.size() != count)
    {
      return fail(error, "City voxel OBJ bridge found an incomplete model hierarchy");
    }

    std::vector<CityVoxelCell> cells;
    for (size_t node = 0; node < count; ++node)
    {
      if (model.hierarchyNodes[node].childCount)
      {
        continue;
      }
      const auto &box = model.centersLengths[node];
      const double span = double(box.length) / pitch;
      const double roundedSpan = std::round(span);
      if (!std::isfinite(span) || roundedSpan < 1.0 || std::abs(span - roundedSpan) > 0.02 || roundedSpan > 1024.0)
      {
        return fail(error, "City voxel OBJ bridge found a leaf off the authored voxel grid");
      }
      const uint64_t side = static_cast<uint64_t>(roundedSpan);
      if (side > maximumExpandedCellsPerModel / side || side * side > maximumExpandedCellsPerModel / side || cells.size() > maximumExpandedCellsPerModel - side * side * side)
      {
        return fail(error, "City voxel OBJ bridge exceeded the expanded cell cap");
      }
      std::array<int32_t, 3> minimum{};
      for (unsigned axis = 0; axis < 3; ++axis)
      {
        if (!cellCoordinate(double(box.center[axis]) - double(box.length) * 0.5, pitch, minimum[axis]))
        {
          return fail(error, "City voxel OBJ bridge found a leaf origin off the authored voxel grid");
        }
      }
      for (unsigned axis = 0; axis < 3; ++axis)
      {
        if (int64_t(minimum[axis]) + int64_t(side) - 1 > std::numeric_limits<int32_t>::max())
        {
          return fail(error, "City voxel OBJ bridge exceeds the integer voxel grid");
        }
      }
      const auto &material = model.materials[node];
      if ((material.albedoRGBA8 >> 24u) != 255u)
      {
        return fail(error, "City voxel OBJ bridge requires opaque source voxels");
      }
      for (uint64_t z = 0; z < side; ++z)
      {
        for (uint64_t y = 0; y < side; ++y)
        {
          for (uint64_t x = 0; x < side; ++x)
          {
            cells.push_back({{{minimum[0] + static_cast<int32_t>(x), minimum[1] + static_cast<int32_t>(y), minimum[2] + static_cast<int32_t>(z)}}, material.albedoRGBA8, material.materialId});
          }
        }
      }
    }
    if (cutouts && instanceIndex == cutouts->targetInstance)
      cells.erase(std::remove_if(cells.begin(), cells.end(),
          [&](const CityVoxelCell &cell) { return cutterCells.find(cell.position) != cutterCells.end(); }),
          cells.end());
    if (cells.empty())
    {
      return fail(error, "City voxel OBJ bridge found no occupied leaves");
    }
    CityVoxelCompiledPlacement placement;
    const uint64_t stableId = firstStableId + static_cast<uint64_t>(instanceIndex);
    if (!compiler.submit(stableId, pitchMicrometres, cells, placement, error))
    {
      return false;
    }
    next.push_back(placement);
  }
  placements.insert(placements.end(), next.begin(), next.end());
  return true;
}

} // namespace virtualgeometry
