#include "editor/virtualgeometry/CityVoxelReleaseAudit.hpp"
#include "json.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc, char **argv)
{
  if (argc != 4)
  {
    std::cerr << "Usage: CityVoxelReleaseAuditTool scene.cvscene settlement.json report.json\n";
    return 2;
  }
  try
  {
    nlohmann::json source;
    {
      std::ifstream input(argv[2]);
      if (!input || !(input >> source))
      {
        throw std::runtime_error("Cannot read authored settlement manifest");
      }
    }
    const auto &audit = source.at("voxelAssemblyAudit");
    if (audit.at("missingBuildingIds").size() != 0u || audit.at("overlappingTriangleSpans").get<uint64_t>() != 0u || audit.at("modelMismatches").get<uint64_t>() != 0u ||
        audit.at("duplicateBuildingIds").get<uint64_t>() != 0u)
    {
      throw std::runtime_error("Authored settlement audit already fails coverage");
    }
    std::vector<virtualgeometry::CityVoxelExpectedOwner> owners;
    for (const auto &owner : audit.at("modelBuildingOwners"))
    {
      owners.push_back({owner.at("id").get<uint64_t>(), owner.at("category").get<std::string>()});
    }
    virtualgeometry::CityVoxelReleaseAuditReport report;
    std::string error;
    const bool buildingAuditPassed = virtualgeometry::auditCityVoxelSceneForRelease(argv[1], owners, {}, 4'000'000u, report, &error);
    const uint64_t unauditedCivilFeatures = audit.at("boundaryFeatures").get<uint64_t>();
    const bool passed = buildingAuditPassed && unauditedCivilFeatures == 0u;
    if (buildingAuditPassed && unauditedCivilFeatures)
    {
      error = "Civil/terrain feature owner manifest is not yet available";
    }
    nlohmann::json result = {
      {"passed", passed},
      {"error", error},
      {"buildingAuditPassed", buildingAuditPassed},
      {"unauditedCivilFeatures", unauditedCivilFeatures},
      {"instances", report.instances},
      {"owners", report.owners},
      {"geometry", report.geometry},
      {"appearances", report.appearances},
      {"occupiedCells", report.occupiedCells},
      {"checkedPages", report.checkedPages},
      {"fallbackCells", report.fallbackCells},
      {"traceAssets", report.traceAssets},
      {"vgAssets", report.vgAssets},
      {"missingOwners", report.missingOwners},
      {"unexpectedOwners", report.unexpectedOwners},
      {"unknownMaterialProfiles", report.unknownMaterialProfiles},
      {"nonOpaqueVoxels", report.nonOpaqueVoxels},
      {"missingOwnerIds", report.missingOwnerIds},
      {"unexpectedOwnerIds", report.unexpectedOwnerIds},
      {"expectedByCategory", report.expectedByCategory},
      {"coveredByCategory", report.coveredByCategory},
      {"missingByCategory", report.missingByCategory}};
    const std::filesystem::path output(argv[3]);
    const auto temporary = output.string() + ".tmp";
    {
      std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
      if (!file || !(file << result.dump(2) << '\n'))
      {
        throw std::runtime_error("Cannot write release audit report");
      }
    }
    std::filesystem::rename(temporary, output);
    std::cout << result.dump(2) << '\n';
    return passed ? 0 : 1;
  }
  catch (const std::exception &e)
  {
    std::cerr << "City voxel release audit: " << e.what() << '\n';
    return 2;
  }
}
