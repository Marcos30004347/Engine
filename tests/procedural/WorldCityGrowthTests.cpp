#include "procedural/world/WorldCityGrowth.hpp"
#include "procedural/world/WorldCityHistoricalGrowth.hpp"

#include <iostream>

using namespace procedural::world::rewrite;

namespace {
std::vector<Position> rect(double x, double z, double w, double h)
{
  return {{x, 0, z}, {x + w, 0, z}, {x + w, 0, z + h}, {x, 0, z + h}};
}

bool samePolygon(const std::vector<Position> &a, const std::vector<Position> &b)
{
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
  return true;
}
}

int main()
{
  int failures = 0;
  auto check = [&](bool condition, const char *message) {
    std::cout << (condition ? "PASS " : "FAIL ") << message << '\n';
    failures += !condition;
  };

  SettlementFabric fabric;
  fabric.revision = 4;
  fabric.polygon.outer = rect(0, 0, 100, 100);
  fabric.layout.envelope.outer = fabric.polygon.outer;
  fabric.layout.suburbs = {rect(100, 30, 45, 35), rect(-35, 40, 35, 30)};
  fabric.gates = {{1, {0, 0, 50}, 1}, {2, {100, 0, 50}, 2}};

  CitySkeleton skeleton;
  skeleton.revision = 4;
  uint64_t id = 1;
  for (int z = 0; z < 4; ++z)
    for (int x = 0; x < 4; ++x)
      skeleton.regions.push_back({id++, 0, 0, CityRegionKind::Block, CityLandUse::UrbanBlock, DistrictUse::Residential, rect(10 + x * 20, 10 + z * 20, 18, 18), false, 324});

  CityCastlePlan castles;
  castles.revision = 4;
  CityCastleTopology castle;
  castle.id = 90;
  castle.precinct = rect(110, 68, 24, 24);
  castle.enclosure = castle.precinct;
  castles.castles.push_back(castle);

  CityGrowthPlan growth;
  std::string error;
  check(planCityGrowth(fabric, skeleton, castles, {}, growth, &error), "growth plan composes");
  check(growth.walls.size() == 1 && growth.walls[0].lifecycle == CityWallLifecycle::Standing, "only the main city curtain is a physical wall");
  check(growth.traces.size() == 2 && growth.traces[0].lifecycle == CityWallLifecycle::Absorbed && growth.traces[1].lifecycle == CityWallLifecycle::Planned,
        "past and future envelopes remain planning traces");
  check(growth.traces.size() == 2 && growth.traces[0].enclosedArea < growth.walls[0].enclosedArea && growth.walls[0].enclosedArea < growth.traces[1].enclosedArea,
        "growth traces remain chronological");
  check(growth.zones.size() == 3, "two suburbs and one castle are explicit growth zones");
  bool enclosed = true;
  for (const auto &zone : growth.zones) enclosed &= zone.insidePlannedWall;
  check(enclosed, "planned outer circuit encloses every growth zone");
  check(!growth.walls[0].gates.empty() && !growth.walls[0].towers.empty(), "standing wall publishes gates and tower stations");

  CityGrowthPlan repeat;
  check(planCityGrowth(fabric, skeleton, castles, {}, repeat, &error) && repeat.walls.size() == growth.walls.size() && samePolygon(repeat.walls.back().boundary, growth.walls.back().boundary) && repeat.traces.size() == growth.traces.size(),
        "growth plan is deterministic");

  CityCompletionPlan occupied;
  occupied.revision=4;
  std::vector<SettlementHistoricalBuilding> records;
  for (unsigned i=0;i<6;++i) {
    const double x=i<2?-5.:i<4?-35.:25.;
    const double z=i%2? -15.:5.;
    occupied.skeleton.regions.push_back({i+1,0,i/2+1,CityRegionKind::Lot,CityLandUse::UrbanBlock,DistrictUse::Residential,rect(x,z,10,10),false,100});
    occupied.historicalLots.push_back({i+1,i+1,64});
    records.push_back({i+1,"home",i<2?10u:100u,1,4});
  }
  CitySmoothedRoute road;road.path={{-60,0,0},{60,0,0}};road.halfWidth=1.5;occupied.routes.push_back(road);
  // The outer era extends above and below as well, leaving the old circuit
  // intact rather than building a coincident replacement wall.
  for(unsigned i=6;i<8;++i) {
    occupied.skeleton.regions.push_back({i+1,0,i+1,CityRegionKind::Lot,CityLandUse::UrbanBlock,DistrictUse::Residential,rect(-5,i==6?-40:30,10,10),false,100});
    occupied.historicalLots.push_back({i+1,i+1,64});records.push_back({i+1,"home",100,1,4});
  }
  std::vector<SettlementHistoricalEnclosure> circuits={
    {10,1,20,true,true,false,true,{1,2}},
    {20,2,120,true,false,false,false,{1,2,3,4,5,6,7,8}},
    {30,3,0,false,false,false,false,{1,2,3,4,5,6,7,8}}};
  CityGrowthPlan historical;
  check(projectHistoricalCityGrowth(records,circuits,occupied,growth,historical,&error),"historical growth projects onto fresh occupied lots");
  check(historical.walls.size()==2 && historical.omissions.empty(),"funded inner and outer circuits coexist without speculative future walls");
  check(historical.epochs.size()==2 && historical.epochs[0].buildings==2 && historical.epochs[1].buildings==8 && historical.epochs[0].blocks.size()<historical.epochs[1].blocks.size(),"new occupied blocks open only with dated building demand");
  check(historical.walls.size()==2 && historical.walls[0].lifecycle==CityWallLifecycle::Absorbed && historical.walls[0].palisade && historical.walls[0].towers.empty(),"retired palisade remains physical without invented stone towers");
  check(historical.walls.size()==2 && historical.walls[0].gates.size()==2 && historical.walls[1].gates.size()==2,"every retained road crossing receives a gate");
  std::reverse(records.begin(),records.end());std::reverse(circuits.begin(),circuits.end());
  CityGrowthPlan shuffled;
  check(projectHistoricalCityGrowth(records,circuits,occupied,growth,shuffled,&error) && shuffled.walls.size()==historical.walls.size() && samePolygon(shuffled.walls[0].boundary,historical.walls[0].boundary),"historical projection is independent of input ordering");
  auto blocked=occupied;
  blocked.skeleton.regions.push_back({99,0,99,CityRegionKind::Lot,CityLandUse::UrbanBlock,DistrictUse::Residential,rect(7,5,5,5),false,25});
  blocked.historicalLots.push_back({99,99,25});
  check(projectHistoricalCityGrowth(records,circuits,blocked,growth,shuffled,&error) && ((shuffled.walls.size()==historical.walls.size() && shuffled.walls[0].enclosedArea>historical.walls[0].enclosedArea) || !shuffled.omissions.empty()),"wall corridor expands around occupied parcels or reports an unresolved conflict");
  circuits.push_back(circuits.back());
  check(!projectHistoricalCityGrowth(records,circuits,occupied,growth,shuffled,&error),"duplicate historical circuits reject transactionally");

  std::cout << "CITY_GROWTH_TESTS failures=" << failures << '\n';
  return failures ? 1 : 0;
}
