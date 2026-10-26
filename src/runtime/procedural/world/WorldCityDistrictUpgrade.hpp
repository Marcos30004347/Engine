#pragma once
#include <set>
#include "WorldCityCompletion.hpp"
#include "WorldCityRelief.hpp"
namespace procedural::world::rewrite {
// Fits a coherent hill surface per block while preserving its street boundary.
// Repeated calls do not layer terrain modifiers.
void planCityHillBlocks(const CityCompletionPlan &, CityReliefPlan &, const CityReliefSettings &);
// Reserves stair courts before dwelling fitting. Unusable candidates remain unchanged.
void upgradeCityDistricts(CityCompletionPlan &, CityReliefPlan &, const CityReliefSettings &, unsigned maximumBlocks=12, const std::set<uint64_t> *occupiedBlocks=nullptr);
}
