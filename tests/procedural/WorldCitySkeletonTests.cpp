#include "json.hpp"
#include "procedural/world/WorldCitySkeleton.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

using namespace procedural::world::rewrite;
using nlohmann::json;
using Poly = std::vector<Position>;

Poly rect(double x, double z, double w, double h)
{
  return {{x, 0, z}, {x + w, 0, z}, {x + w, 0, z + h}, {x, 0, z + h}};
}

json point(const Position &p)
{
  return {p.x, p.y, p.z};
}

json poly(const Poly &p)
{
  json result = json::array();
  for (const auto &v : p)
    result.push_back(point(v));
  return result;
}

json encode(const CitySkeleton &s)
{
  json j;
  j["regions"] = json::array();
  j["contacts"] = json::array();
  j["roads"] = json::array();
  j["roadNodes"] = json::array();
  j["walls"] = json::array();
  j["gates"] = json::array();
  for (const auto &r : s.regions)
    j["regions"].push_back({{"id", r.id}, {"parent", r.parent}, {"kind", cityRegionKindName(r.kind)}, {"use", cityLandUseName(r.landUse)}, {"boundary", poly(r.boundary)}});
  for (const auto &c : s.regionContacts)
    j["contacts"].push_back({{"id", c.id}, {"a", c.a}, {"b", c.b}, {"kind", int(c.kind)}, {"point", point(c.point)}, {"length", c.sharedLength}});
  for (const auto &e : s.roadEdges)
    j["roads"].push_back({{"id", e.id}, {"from", e.from}, {"to", e.to}, {"kind", int(e.kind)}, {"width", e.halfWidth}, {"path", poly(e.path)}});
  for (const auto &n : s.roadNodes)
    j["roadNodes"].push_back({{"id", n.id}, {"kind", int(n.kind)}, {"position", point(n.position)}, {"degree", n.edges.size()}});
  for (const auto &e : s.wallEdges)
    j["walls"].push_back({{"id", e.id}, {"from", e.from}, {"to", e.to}, {"a", point(e.a)}, {"b", point(e.b)}});
  for (const auto &g : s.gates)
    j["gates"].push_back({{"id", g.id}, {"wallNode", g.wallNode}, {"roadNode", g.roadNode}, {"position", point(g.position)}});
  return j;
}

int main(int argc, char **argv)
{
  int checks = 0, failures = 0;
  auto check = [&](bool ok, const std::string &message)
  {
    ++checks;
    failures += !ok;
    std::cout << (ok ? "PASS " : "FAIL ") << message << '\n';
  };

  CityTerritory territory;
  territory.revision = 12;
  territory.regions.districts = {
      {10, 1, DistrictUse::Civic, {1}, {rect(0, 0, 50, 100)}, {}, {}, 5000, 0},
      {11, 2, DistrictUse::Residential, {2}, {rect(50, 0, 50, 100)}, {}, {}, 5000, 0}};
  AgriculturalPatch patch;
  patch.id = 20;
  patch.source = 1;
  patch.boundary = rect(110, 0, 90, 100);
  patch.access = {{100, 0, 0}, {110, 0, 10}, {155, 0, 50}};
  territory.rural.patches = {patch};
  FarmField fieldA{21, 20, 1, rect(112, 2, 41, 96), {}, 3936, 0, 0, 0};
  FarmField fieldB{22, 20, 1, rect(155, 2, 41, 96), {}, 3936, 0, 0, 0};
  FarmFieldProduct fields;
  fields.fields = {fieldA, fieldB};
  territory.fields = {fields};
  territory.holdings = {{20, 1, rect(130, 35, 20, 20), {{110, 0, 10}, {130, 0, 45}}, {}, 400}};

  SettlementFabric fabric;
  fabric.revision = 13;
  CityStreetBlock left, right;
  left.id = 30;
  left.district = 10;
  left.use = DistrictUse::Civic;
  left.boundary.outer = rect(4, 4, 42, 92);
  left.lots = {rect(6, 6, 18, 40), rect(24, 6, 20, 40)};
  right.id = 31;
  right.district = 11;
  right.use = DistrictUse::Residential;
  right.boundary.outer = rect(54, 4, 42, 92);
  right.lots = {rect(56, 6, 18, 40), rect(74, 6, 20, 40)};
  fabric.layout.blocks = {left, right};
  CityStreet street;
  street.id = 40;
  street.a = {0, 0, 0};
  street.b = {100, 0, 50};
  street.path = {{0, 0, 0}, {50, 0, 50}, {100, 0, 50}};
  street.halfWidth = 2.5;
  street.arterial = true;
  fabric.layout.streets = {street};
  fabric.approaches = {{{-30, 0, 0}, {0, 0, 0}}};

  CityWalls walls;
  walls.revision = 4;
  walls.circuits = {{1, rect(0, 0, 100, 100), 400, 4}};
  walls.gates = {{50, 1, {40}, {-1, 0, 0}, {1, 0, 0}}};

  CitySkeleton skeleton;
  std::string error;
  check(composeCitySkeleton(territory, &walls, fabric, skeleton, &error), "compose unified city skeleton: " + error);
  check(skeleton.regions.size() == 12, "district, field, farmyard, block and lot polygons retain identity");
  {
    // A holding with a farmhouse publishes the farmhouse's site as a lot of
    // the farmstead programme, inside the yard, so the city builds a real
    // farmhouse there and a farmer's household can be bound to it.
    auto farmed = territory;
    farmed.holdings.front().buildings = {{1, FarmBuildingUse::Farmhouse, rect(133, 38, 10, 7), {138, 0, 38}, 0, 7},
                                         {2, FarmBuildingUse::Barn, rect(133, 48, 12, 6), {139, 0, 48}, 0, 6}};
    CitySkeleton withFarm;
    const bool composed = composeCitySkeleton(farmed, nullptr, fabric, withFarm, &error);
    const CitySkeletonRegion *lot = nullptr, *yard = nullptr;
    for (const auto &r : withFarm.regions)
    {
      if (r.kind == CityRegionKind::Farmyard) yard = &r;
      if (r.kind == CityRegionKind::Lot && r.lotProgramme == "farmstead") lot = &r;
    }
    bool insideYard = lot != nullptr && yard != nullptr && lot->parent == yard->id;
    if (insideYard)
    {
      for (const auto &p : lot->boundary)
      {
        bool in = false;
        for (size_t i = 0, j = yard->boundary.size() - 1; i < yard->boundary.size(); j = i++)
        {
          const auto a = yard->boundary[i], b = yard->boundary[j];
          if ((a.z > p.z) != (b.z > p.z) && p.x < (b.x - a.x) * (p.z - a.z) / (b.z - a.z) + a.x) in = !in;
        }
        insideYard = insideYard && in;
      }
    }
    check(composed && insideYard && lot->landUse == CityLandUse::Farmstead, "a holding's farmhouse is a farmstead lot inside its yard");
    check(composed && withFarm.regions.size() == skeleton.regions.size() + 1, "the barn stays a massing; only the farmhouse becomes a lot");
  }
  for(const auto &region:skeleton.regions)if(region.kind==CityRegionKind::Block)
    check(region.blockStrategy!=CityBlockStrategy::Unplanned,"every block records a planning strategy");
  {
    SettlementFabric plannedFabric;plannedFabric.revision=1;
    auto block=right;block.id=32;block.boundary.outer=rect(0,0,46,38);block.lots.clear();plannedFabric.layout.blocks={block};
    for(size_t i=0;i<4;++i) {
      CityStreet way;way.id=i+1;way.a=block.boundary.outer[i];way.b=block.boundary.outer[(i+1)%4];way.halfWidth=2;
      plannedFabric.layout.streets.push_back(way);
    }
    CitySkeleton planned;
    check(composeCitySkeleton({},nullptr,plannedFabric,planned,&error),"production skeleton plans a complete courtyard block");
    check(!planned.publicSpaces.empty() && planned.alleys.size()>=2,"production skeleton retains paving and internal access");
    unsigned attached=0;
    for(const auto &r:planned.regions)if(r.kind==CityRegionKind::Lot) {
      check(!r.lotProgramme.empty(),"lot programme survives the block adapter");attached+=r.attached;
    }
    check(attached>4,"production block carries attached-house policy");
    plannedFabric.layout.blocks.front().use=DistrictUse::Suburb;
    CityWalls enclosure;CityWallCircuit circuit;circuit.era=1;circuit.boundary=rect(-5,-5,60,55);enclosure.circuits.push_back(circuit);
    check(composeCitySkeleton({},&enclosure,plannedFabric,planned,&error),"suburban programme inside walls receives urban planning");
    check(std::none_of(planned.regions.begin(),planned.regions.end(),[](const auto &r){return r.blockStrategy==CityBlockStrategy::Crofts;}),
          "enclosed land is not spent on agricultural crofts");
    check(composeCitySkeleton({},nullptr,plannedFabric,planned,&error) &&
          std::any_of(planned.regions.begin(),planned.regions.end(),[](const auto &r){return r.blockStrategy==CityBlockStrategy::Crofts;}),
          "open suburbs retain their working yards and crofts");
  }
  {
    SettlementFabric packed;packed.revision=1;
    CityStreetBlock block;block.id=910;block.use=DistrictUse::Residential;
    block.boundary.outer=rect(0,0,24,12);block.frontagePacked=true;
    block.lots={rect(0,0,8,10),rect(8,0,8,10),rect(16,0,8,10)};
    packed.layout.blocks={block};
    CityStreet road;road.id=1;road.a={0,0,-2};road.b={24,0,-2};road.halfWidth=2;
    packed.layout.streets={road};CitySkeleton result;
    check(composeCitySkeleton({},nullptr,packed,result,&error),"canonical packed parcels compose");
    unsigned lots=0;double area=0;
    for(const auto &r:result.regions)if(r.kind==CityRegionKind::Lot){++lots;area+=r.area;}
    check(lots==3 && std::abs(area-240)<1e-6,
          "later skeleton preserves packed parcels instead of replacing them with another layout");
    check(std::count_if(result.regionContacts.begin(),result.regionContacts.end(),[](const auto &c){
      return c.kind==CityRegionContactKind::SharedBoundary && std::abs(c.sharedLength-10)<1e-6;
    })==2,"repacked neighbouring houses receive fresh shared-boundary contacts");
  }
  {
    SettlementFabric packed;packed.revision=1;
    CityStreetBlock block;block.id=911;block.use=DistrictUse::Residential;
    block.boundary.outer=rect(0,0,46,38);block.frontagePacked=true;
    block.lots={rect(0,0,20,10),rect(20,0,20,10)};
    packed.layout.blocks={block};
    for(size_t i=0;i<4;++i) {
      CityStreet road;road.id=i+1;road.a=block.boundary.outer[i];road.b=block.boundary.outer[(i+1)%4];road.halfWidth=2;
      packed.layout.streets.push_back(road);
    }
    CitySkeleton inward;
    check(composeCitySkeleton({},nullptr,packed,inward,&error),"selected packed block receives a planned courtyard");
    unsigned facing=0,courts=0;
    for(const auto &r:inward.regions)if(r.kind==CityRegionKind::Lot)facing+=r.courtFacing;
    for(const auto &space:inward.publicSpaces)if(space.kind==CityPublicSpace::Kind::Court && space.rise==0)courts++;
    check(facing>=4 && courts>0 && inward.alleys.size()>=2,
          "courtyard parcels carry inward entry policy and two accessible paved approaches");
  }
  std::set<uint64_t> regionIds;
  for (const auto &r : skeleton.regions)
    regionIds.insert(r.id);
  check(regionIds.size() == skeleton.regions.size(), "region IDs are globally unique");
  bool parents = true;
  for (const auto &r : skeleton.regions)
    parents &= !r.parent || regionIds.count(r.parent);
  check(parents, "every child names an existing parent region");
  check(std::count_if(skeleton.regionContacts.begin(), skeleton.regionContacts.end(), [](const auto &c) { return c.kind == CityRegionContactKind::Contains; }) == 9, "containment graph covers blocks, fields, yard and lots");
  check(std::any_of(skeleton.regionContacts.begin(), skeleton.regionContacts.end(), [](const auto &c) { return c.kind == CityRegionContactKind::SharedBoundary && c.sharedLength > 90; }), "peer region graph records exact shared boundary length");
  check(skeleton.roadEdges.size() == 5 && skeleton.wallEdges.size() == 4, "street, approach, farm track and wall circuit share graph vocabulary");
  check(skeleton.gates.size() == 1 && skeleton.gates[0].wallNode && skeleton.gates[0].roadNode, "gate connects wall and road graphs");
  {
    auto offsetWalls = walls;
    offsetWalls.gates[0].a = {49, 0, 100};
    offsetWalls.gates[0].b = {51, 0, 100};
    CitySkeleton unserved;
    check(composeCitySkeleton(territory, &offsetWalls, fabric, unserved, &error), "record gate without an authored approach");
    check(unserved.roadEdges.size() == skeleton.roadEdges.size(), "unserved gate does not invent a straight approach road");
    const auto node = std::find_if(unserved.roadNodes.begin(), unserved.roadNodes.end(), [&](const auto &n) { return n.id == unserved.gates.front().roadNode; });
    check(node != unserved.roadNodes.end() && node->edges.empty(), "unserved gate remains explicitly disconnected");
    offsetWalls.gates[0].a = {24, 0, 25};
    offsetWalls.gates[0].b = {26, 0, 25};
    CitySkeleton contact;
    check(composeCitySkeleton(territory, &offsetWalls, fabric, contact, &error), "record gate at existing road interior");
    const auto crossing = std::find_if(contact.roadNodes.begin(), contact.roadNodes.end(), [&](const auto &n) { return n.id == contact.gates.front().roadNode; });
    check(crossing != contact.roadNodes.end() && crossing->edges.size() == 2, "gate on planned road splits it without a shortcut");
  }

  auto permutedTerritory = territory;
  std::reverse(permutedTerritory.regions.districts.begin(), permutedTerritory.regions.districts.end());
  auto permutedFabric = fabric;
  std::reverse(permutedFabric.layout.blocks.begin(), permutedFabric.layout.blocks.end());
  CitySkeleton permuted;
  check(composeCitySkeleton(permutedTerritory, &walls, permutedFabric, permuted, &error), "compose permuted inputs: " + error);
  std::vector<uint64_t> idsA, idsB;
  for (const auto &r : skeleton.regions)
    idsA.push_back(r.id);
  for (const auto &r : permuted.regions)
    idsB.push_back(r.id);
  check(idsA == idsB, "stable IDs do not depend on input order");

  {
    SettlementFabric junctionFabric;
    const auto road = [&](uint64_t id, Position a, Position b)
    {
      CityStreet s;
      s.id = id; s.a = a; s.b = b; s.path = {a, b}; s.halfWidth = 1;
      junctionFabric.layout.streets.push_back(s);
    };
    road(1, {0,0,0}, {20,0,0});
    road(2, {5,0,0}, {5,0,10});
    road(3, {10,0,-10}, {10,0,10});
    road(4, {15,5,-10}, {15,5,10});
    CitySkeleton junctions;
    check(composeCitySkeleton({}, nullptr, junctionFabric, junctions, &error), "compose T, crossing and grade-separated roads");
    const auto degree = [&](double x, double z)
    {
      for (const auto &n : junctions.roadNodes)
        if (n.position.x == x && n.position.z == z && n.position.y == 0)
          return n.edges.size();
      return size_t(0);
    };
    check(degree(5,0) == 3, "T-junction connects to the middle of the main street");
    check(degree(10,0) == 4, "crossing streets share a routing junction");
    check(degree(15,0) == 0, "grade-separated crossing does not create a junction");
  }

  if (argc > 1)
  {
    std::filesystem::path output = argv[1];
    std::filesystem::create_directories(output);
    std::ofstream(output / "city-skeleton-c1.json") << encode(skeleton).dump(2);
  }
  std::cout << checks << " checks, " << failures << " failures\n";
  return failures ? 1 : 0;
}
