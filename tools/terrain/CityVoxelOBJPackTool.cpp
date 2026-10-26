#include "editor/virtualgeometry/CityVoxelReleaseAudit.hpp"
#include "editor/virtualgeometry/CityVoxelScenePackager.hpp"
#include "editor/virtualgeometry/CityVoxelSparseSceneBridge.hpp"
#include "editor/voxel/SparseVoxelOBJAnalysis.hpp"
#include "editor/voxel/SparseVoxelOBJBuilder.hpp"
#include "virtualgeometry/CityVoxelSceneRuntime.hpp"
#include "virtualgeometry/CityVoxelStaticScene.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <unordered_map>

int main(int argc, char **argv)
{
  using namespace voxel::svraster;
  using namespace virtualgeometry;
  if (argc != 4 && argc != 5)
  {
    std::cerr << "Usage: CityVoxelOBJPackTool source.obj output.cvscene owner-id [--split-shapes]\n";
    return 2;
  }
  const bool splitShapes = argc == 5 && std::string(argv[4]) == "--split-shapes";
  if (argc == 5 && !splitShapes)
  {
    std::cerr << "Unknown option: " << argv[4] << '\n';
    return 2;
  }
  std::string error;
  uint64_t owner = 0;
  try
  {
    owner = std::stoull(argv[3]);
  }
  catch (...)
  {
    std::cerr << "Owner ID must be a positive integer\n";
    return 2;
  }
  if (!owner)
  {
    std::cerr << "Owner ID must be nonzero\n";
    return 2;
  }
  SparseVoxelOBJAnalysis analysis;
  if (!analyzeSparseVoxelOBJ(argv[1], analysis, &error))
  {
    std::cerr << error << '\n';
    return 1;
  }
  SparseVoxelOBJImportSettings import;
  import.voxelSizeMeters = 0.1;
  import.splitByShape = splitShapes;
  import.maximumEstimatedCells = 5'000'000;
  import.maximumEstimatedTemporaryBytes = 256ull << 20u;
  SparseVoxelScene source;
  if (!buildSparseVoxelOBJScene(analysis, import, source, nullptr, &error))
  {
    std::cerr << error << '\n';
    return 1;
  }

  CityVoxelAssetCompiler compiler;
  std::vector<CityVoxelCompiledPlacement> compiled;
  CityVoxelOverlapCutouts cutouts;
  const CityVoxelOverlapCutouts *cutoutPolicy = nullptr;
  if (splitShapes)
  {
    std::ifstream obj(argv[1]);
    std::vector<std::string> shapeNames;
    std::string line;
    while (std::getline(obj, line))
      if (line.rfind("o ", 0) == 0) shapeNames.push_back(line.substr(2));
    const bool hasOpeningNames=std::any_of(shapeNames.begin(),shapeNames.end(),
        [](const std::string &name) {
          return name.rfind("door_",0)==0 || name.rfind("window_",0)==0;
        });
    if (hasOpeningNames && shapeNames.size()!=source.instances.size())
    {
      std::cerr << "Opening part order differs from voxelized OBJ shape order\n";
      return 1;
    }
    if (shapeNames.size() == source.instances.size())
    {
      for (uint32_t i=0; i<shapeNames.size(); ++i)
      {
        if (shapeNames[i] == "shell") cutouts.targetInstance=i;
        if (shapeNames[i].rfind("door_",0)==0 || shapeNames[i].rfind("window_",0)==0)
          cutouts.cutterInstances.push_back(i);
      }
      if (cutouts.targetInstance != UINT32_MAX && !cutouts.cutterInstances.empty())
        cutoutPolicy=&cutouts;
    }
    if (hasOpeningNames && !cutoutPolicy)
    {
      std::cerr << "Opening parts require one shell and at least one frame cutout\n";
      return 1;
    }
  }
  if (!compileCityVoxelSparseScene(source, compiler, 100000u, 1'000'000u, 1u, compiled, &error, cutoutPolicy))
  {
    std::cerr << error << '\n';
    return 1;
  }
  std::unordered_map<uint64_t, uint32_t> geometryIndex, appearanceIndex;
  std::vector<CityVoxelStaticModel> models;
  std::vector<uint64_t> appearances;
  for (const auto &geometry : compiler.geometry())
  {
    geometryIndex.emplace(geometry.id, static_cast<uint32_t>(models.size()));
    models.push_back({geometry.id, "city-obj-" + std::to_string(geometry.id), 0u});
  }
  for (const auto &appearance : compiler.appearances())
  {
    appearanceIndex.emplace(appearance.id, static_cast<uint32_t>(appearances.size()));
    appearances.push_back(appearance.id);
  }
  std::vector<CityVoxelStaticPlacement> placements;
  std::unordered_map<uint64_t, uint64_t> owners;
  std::array<int32_t, 3> minimum{INT32_MAX, INT32_MAX, INT32_MAX};
  std::array<int32_t, 3> maximum{INT32_MIN, INT32_MIN, INT32_MIN};
  for (const auto &part : compiled)
  {
    const auto &geometry = compiler.geometry().at(geometryIndex.at(part.geometryId));
    placements.push_back({part.stableId, geometryIndex.at(part.geometryId), appearanceIndex.at(part.appearanceId), 0u, part.originCell});
    owners.emplace(part.stableId, owner);
    for (unsigned axis = 0; axis < 3; ++axis)
    {
      minimum[axis] = std::min(minimum[axis], part.originCell[axis]);
      maximum[axis] = std::max(maximum[axis], part.originCell[axis] + geometry.extent[axis]);
    }
  }
  std::set<uint32_t> profiles;
  for (const auto &appearance : compiler.appearances())
  {
    std::vector<CityVoxelCell> values;
    if (!decodeCityVoxelAppearance(appearance, values, &error))
    {
      std::cerr << error << '\n';
      return 1;
    }
    for (const auto &value : values)
    {
      profiles.insert(value.materialProfile);
    }
  }
  std::vector<CityVoxelMaterialReference> materials;
  for (uint32_t profile : profiles)
  {
    materials.push_back({profile, 0u, 0x4349545900000000ull | (uint64_t(profile) + 1ull)});
  }

  CityVoxelStaticScene city;
  if (!city.load(std::move(models), std::move(appearances), {{{0, 0, 0}, 0u}}, {{minimum, maximum, 0u, static_cast<uint32_t>(placements.size())}}, std::move(placements), 100000u, &error) ||
      !writeCityVoxelCompiledScene(argv[2], city, compiler.geometry(), compiler.appearances(), materials, analysis.sourceContentHash, source.buildSettingsHash, {}, &error, &owners))
  {
    std::cerr << error << '\n';
    return 1;
  }
  CityVoxelReleaseAuditReport verified;
  if (!auditCityVoxelSceneForRelease(argv[2], {{owner, "imported-mesh"}}, {}, 1'000'000u, verified, &error))
  {
    std::cerr << "Packed scene failed integrity audit: " << error << '\n';
    return 1;
  }
  CityVoxelSceneRuntime runtime;
  if (!runtime.open(argv[2], {}, {}, &error))
  {
    std::cerr << "Packed scene failed clean runtime publication: " << error << '\n';
    return 1;
  }
  const uint64_t maximumFrames=2u+runtime.index().pages.size()/
      std::max(1u,runtime.budgets().maximumReadsPerFrame);
  for(uint64_t frame=1u;frame<=maximumFrames && runtime.statistics().detailedCells!=1u;++frame)
    if(!runtime.update(frame,[](uint32_t,const CityVoxelSceneRuntime::Bounds &){return true;},&error)) {
      std::cerr << "Packed scene failed clean runtime publication: " << error << '\n';
      return 1;
    }
  if(runtime.statistics().detailedCells!=1u || runtime.statistics().activeInstances!=compiled.size()) {
    std::cerr << "Packed scene exceeded publication reads or budgets\n";
    return 1;
  }
  std::cout << "Packed " << city.placements().size() << " placements, " << compiler.geometry().size() << " geometries, " << compiler.appearances().size() << " appearances into " << argv[2] << " ("
            << verified.occupiedCells << " cells, " << verified.checkedPages << " checked pages)\n";
  std::cout << "Shared geometry reuses: " << compiler.report().geometryReuses << '\n';
  const auto &resident = runtime.statistics();
  std::cout << "Resident bytes: geometry=" << resident.geometryEncodedBytes << " appearance=" << resident.appearanceBytes << " trace=" << resident.traceBytes << " virtualGeometry=" << resident.virtualGeometryBytes
            << " fallback=" << resident.fallbackBytes << '\n';
  return 0;
}
