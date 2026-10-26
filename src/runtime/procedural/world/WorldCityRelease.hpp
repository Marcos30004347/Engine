#pragma once

#include "WorldCityComposition.hpp"
#include "WorldCityModel.hpp"

namespace procedural::world::rewrite {

// Publication gate, not a replacement for the planners' geometry validators.
// Diagnostics remain exportable when publication is refused.
struct CityReleaseReport
{
  bool accepted = false;
  size_t requested = 0, generated = 0;
  std::vector<uint64_t> missingBuildings;
  std::vector<std::string> failures;
};

inline CityReleaseReport
assessCityRelease(const CityCompletionPlan &city, const CityReliefPlan &relief, const CityModelPlan &model, const std::optional<std::vector<SettlementHistoricalBuilding>> &history, const std::vector<bool> &runtimeTiles)
{
  CityReleaseReport report;
  auto fail = [&](const char *s) {
    report.failures.emplace_back(s);
  };
  if (!city.revision || city.revision != relief.revision || city.revision != model.revision)
  {
    fail("revision-mismatch");
  }
  if (!city.plotContracts || !city.composition || !relief.constructionTerrain)
  {
    fail("accepted-blueprint-missing");
  }
  else
  {
    if (city.plotContracts->revision != city.revision || city.composition->revision != city.revision || city.plotContracts->terrain != relief.constructionTerrain || city.composition->plots != city.plotContracts ||
        city.composition->structures != city.structureContracts)
    {
      fail("blueprint-snapshot-mismatch");
    }
    if (!city.plotContracts->unmet.empty())
    {
      fail("unmet-plot-demand");
    }
    if (city.plotContracts->plots.size() != city.composition->buildings.size())
    {
      fail("incomplete-composition");
    }
  }
  if (runtimeTiles.empty() || std::any_of(runtimeTiles.begin(), runtimeTiles.end(), [](bool built) {
        return !built;
      }))
  {
    fail("incomplete-runtime-terrain");
  }
  if (model.mesh.triangles.empty())
  {
    fail("empty-native-mesh");
  }
  if (!history)
  {
    fail("historical-demand-missing");
  }
  else
  {
    report.requested = history->size();
    std::set<uint64_t> requested, physical, bound;
    std::map<uint64_t, uint64_t> bindings;
    for (const auto &b : model.buildings)
    {
      if (!b.id || !physical.insert(b.id).second)
      {
        fail("duplicate-physical-building");
      }
    }
    for (const auto &binding : city.historicalLots)
    {
      if (!bindings.emplace(binding.building, binding.lot).second)
      {
        fail("duplicate-historical-binding");
      }
    }
    for (const auto &b : *history)
    {
      if (!b.id || !requested.insert(b.id).second)
      {
        fail("invalid-historical-identity");
        continue;
      }
      auto it = bindings.find(b.id);
      if (it == bindings.end() || !it->second || !physical.count(it->second))
      {
        report.missingBuildings.push_back(b.id);
      }
      else if (!bound.insert(it->second).second)
      {
        fail("historical-buildings-share-one-mesh");
      }
      else
      {
        ++report.generated;
        if (city.plotContracts)
        {
          const auto &plots = city.plotContracts->plots;
          auto contract = std::find_if(plots.begin(), plots.end(), [&](const auto &plot) {
            return plot.building == it->second;
          });
          if (contract == plots.end() || contract->brief.floors != b.floors)
          {
            fail("historical-programme-mismatch");
          }
          else
          {
            using Class = generator::BuildingClass;
            const auto kind = contract->brief.building;
            if ((b.use == "keep" && kind != Class::Castle) || (b.use == "manor" && kind != Class::ManorHouse) || (b.use == "inn" && kind != Class::Inn))
            {
              fail("historical-programme-mismatch");
            }
          }
        }
      }
    }
    for (const auto &b : model.buildings)
    {
      const bool auxiliary = b.kind == CityModelBuildingKind::CastleRoom || b.kind == CityModelBuildingKind::DefenceTower || b.kind == CityModelBuildingKind::Gatehouse;
      if (!auxiliary && !bound.count(b.id))
      {
        fail("unrequested-occupied-building");
      }
    }
    for (const auto &[id, lot] : bindings)
    {
      if (!requested.count(id))
      {
        fail("invented-historical-binding");
      }
    }
    if (!report.missingBuildings.empty())
    {
      fail("historical-building-unresolved");
    }
  }
  std::sort(report.failures.begin(), report.failures.end());
  report.failures.erase(std::unique(report.failures.begin(), report.failures.end()), report.failures.end());
  std::sort(report.missingBuildings.begin(), report.missingBuildings.end());
  report.accepted = report.failures.empty();
  return report;
}
} // namespace procedural::world::rewrite
