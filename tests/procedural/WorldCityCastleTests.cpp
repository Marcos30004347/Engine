#include "procedural/world/WorldCityCastle.hpp"

#include <iostream>

using namespace procedural::world;
using namespace procedural::world::rewrite;

int main()
{
  int checks = 0, failures = 0;
  const auto check = [&](bool ok, const char *message)
  {
    ++checks;
    failures += !ok;
    std::cout << (ok ? "PASS " : "FAIL ") << message << '\n';
  };
  SettlementFabric fabric;
  fabric.revision = 7;
  fabric.polygon.outer = {{0, 0, 0}, {100, 0, 0}, {100, 0, 100}, {0, 0, 100}};
  generator::CompoundPlan compound;
  compound.id = 41;
  compound.kind = generator::CompoundKind::Castle;
  compound.solved = true;
  compound.precinct = {{0, 0, 30}, {30, 0, 30}, {30, 0, 70}, {0, 0, 70}};
  compound.enclosure = compound.precinct;
  compound.gate = {0, 0, 50};
  fabric.compounds = {compound};

  CitySkeleton skeleton;
  skeleton.revision = 8;
  skeleton.wallEdges = {{11, 1, 1, 2, {0, 0, 0}, {0, 0, 100}}};
  skeleton.roadNodes = {{21, CityRoadNodeKind::Gate, {5, 0, 50}, {}}, {22, CityRoadNodeKind::Endpoint, {-20, 0, 50}, {}}};
  CityCastlePlan plan;
  std::string error;
  check(composeCityCastles(fabric, skeleton, {}, plan, &error), "compose castle topology");
  check(plan.castles.size() == 1 && plan.castles[0].mode == CityCastleMode::WallAttached, "exact shared wall selects wall-attached mode");
  check(plan.castles[0].sharedWallEdges == std::vector<uint64_t>{11}, "castle owns the shared wall interval by edge ID");
  check(plan.castles[0].cityAccessNode && plan.castles[0].exteriorAccessNode && plan.castles[0].cityAccessNode != plan.castles[0].exteriorAccessNode,
        "wall-attached castle exposes distinct city and exterior access nodes");

  auto detached = fabric;
  detached.compounds[0].enclosure = {{20, 0, 30}, {40, 0, 30}, {40, 0, 70}, {20, 0, 70}};
  CityCastleSettings forced;
  forced.modes[41] = CityCastleMode::WallAttached;
  check(!composeCityCastles(detached, skeleton, forced, plan, &error), "wall-attached mode rejects a clipping/non-contact enclosure");
  std::cout << checks << " checks, " << failures << " failures\n";
  return failures ? 1 : 0;
}
