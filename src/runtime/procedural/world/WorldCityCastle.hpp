#pragma once

#include "WorldCitySkeleton.hpp"

#include <map>

namespace procedural::world::rewrite {
enum class CityCastleMode : uint8_t
{
  InteriorKeep,
  WallAttached,
  OutsideGate,
  DetachedHillfort,
  HarbourFort
};

struct CityCastleSettings
{
  std::map<uint64_t, CityCastleMode> modes;
  double wallContactTolerance = .05;
};

struct CityCastleTopology
{
  uint64_t id = 0, compound = 0;
  CityCastleMode mode = CityCastleMode::InteriorKeep;
  std::vector<Position> precinct, enclosure;
  std::vector<uint64_t> sharedWallEdges;
  uint64_t cityAccessNode = 0, exteriorAccessNode = 0;
  Position gate;
};

struct CityCastlePlan
{
  uint64_t revision = 0;
  std::vector<CityCastleTopology> castles;
};

// Adapts solved castle compounds to the canonical city topology. A requested
// WallAttached castle must share an exact collinear interval with the wall
// graph and expose distinct city/exterior road contacts.
bool composeCityCastles(
    const SettlementFabric &,
    const CitySkeleton &,
    const CityCastleSettings &,
    CityCastlePlan &,
    std::string *error = nullptr);

const char *cityCastleModeName(CityCastleMode);
} // namespace procedural::world::rewrite
