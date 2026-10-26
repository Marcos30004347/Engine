#undef NDEBUG
#include "editor/virtualgeometry/CityVoxelSparseSceneBridge.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <set>

namespace svr = voxel::svraster;

int main()
{
  svr::SparseVoxelScene source;
  svr::SparseVoxelBLAS model;
  model.hierarchyNodes.push_back({0u, 0u, 0u, 1u});
  model.centersLengths.push_back({{0.1f, 0.1f, 0.1f}, 0.2f});
  model.materials.push_back({0xff3366aau, 0u, 0u, 0u, 17u, 0u, 0u, 0u});
  source.models.push_back(std::move(model));
  source.instances.push_back({0u, 0u, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}});
  svr::CityVoxelAssetCompiler compiler;
  std::vector<svr::CityVoxelCompiledPlacement> placements;
  std::string error;
  assert(virtualgeometry::compileCityVoxelSparseScene(source, compiler, 100000u, 8u, 1u, placements, &error));
  assert(placements.size() == 1u && compiler.geometry().size() == 1u && compiler.appearances().size() == 1u);
  std::vector<svr::CityVoxelCell> decoded;
  assert(svr::decodeCityVoxelAppearance(compiler.appearances()[0], decoded, &error));
  assert(decoded.size() == 8u);
  for (const auto &cell : decoded)
  {
    assert(cell.rgba == 0xff3366aau && cell.materialProfile == 17u);
  }
  assert(!virtualgeometry::compileCityVoxelSparseScene(source, compiler, 100000u, 7u, 2u, placements, &error));
  assert(placements.size() == 1u);
  svr::SparseVoxelBLAS frame;
  frame.hierarchyNodes.push_back({0u, 0u, 0u, 1u});
  frame.centersLengths.push_back({{0.05f, 0.05f, 0.05f}, 0.1f});
  frame.materials.push_back({0xff885522u, 0u, 0u, 0u, 18u, 0u, 0u, 0u});
  source.models.push_back(std::move(frame));
  source.instances.push_back({1u, 0u, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}});
  svr::CityVoxelAssetCompiler carved;
  std::vector<svr::CityVoxelCompiledPlacement> carvedPlacements;
  virtualgeometry::CityVoxelOverlapCutouts cutouts{0u,{1u}};
  assert(virtualgeometry::compileCityVoxelSparseScene(source, carved, 100000u, 8u,
      10u, carvedPlacements, &error, &cutouts));
  assert(carvedPlacements.size()==2u);
  std::set<std::array<int32_t,3>> occupied;
  for (const auto &placement : carvedPlacements) {
    const auto &geometry=*std::find_if(carved.geometry().begin(), carved.geometry().end(),
        [&](const auto &asset){return asset.id==placement.geometryId;});
    std::vector<std::array<int32_t,3>> positions;
    assert(svr::decodeCityVoxelGeometry(geometry, positions, nullptr, &error));
    for (auto cell : positions) {
      for (unsigned axis=0;axis<3;++axis) cell[axis]+=placement.originCell[axis];
      assert(occupied.insert(cell).second);
    }
  }
  assert(occupied.size()==8u);
  std::cout << "City voxel sparse-scene bridge tests passed\n";
}
