#pragma once
#include "WorldCityCompletion.hpp"
#include "WorldCityStructures.hpp"

namespace procedural::world::rewrite {
inline bool applyCityStructures(const CityStructurePlan &structures, CityCompletionPlan &city, std::string *error = nullptr)
{
  const auto fail = [&](const char *why) {
    if (error)
    {
      *error = why;
    }
    return false;
  };
  if (!structures.revision || structures.revision != city.revision || !city.plotContracts || structures.plots != city.plotContracts || structures.mesh.triangles.empty())
  {
    return fail("structures require the exact accepted city plot snapshot");
  }
  // Legacy boundary assemblies were not checked against these reservations.
  // They cannot coexist until a parent planner explicitly migrates them.
  if (!city.wallSegments.empty() || !city.towers.empty() || !city.gatehouses.empty())
  {
    return fail("legacy boundaries must be replanned before structural publication");
  }
  city.structureContracts = std::make_shared<const CityStructurePlan>(structures);
  if (error)
  {
    error->clear();
  }
  return true;
}
} // namespace procedural::world::rewrite
