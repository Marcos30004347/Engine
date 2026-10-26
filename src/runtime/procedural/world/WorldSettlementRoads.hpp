#pragma once
#include "WorldRoadNetwork.hpp"
#include "WorldSuitability.hpp"
namespace procedural::world::rewrite
{
// Uses final Step 06/07 terrain, continuous river/face exclusions and the Step 08
// water/flood mask. Unresolved water crossings remain forbidden until Step 10.
bool buildSettlementRoads(const WorldSuitability &, const SettlementProduct &, const WorldPhysicalTerrain &, const WorldRiverChannels &, const RoadSettings &, RoadNetworkProduct &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
