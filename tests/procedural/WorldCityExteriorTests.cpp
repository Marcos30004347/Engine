#include "json.hpp"
#include "procedural/world/WorldCityExterior.hpp"

#include <algorithm>
#include <cmath>
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

json poly(const Poly &p)
{
  json a = json::array();
  for (const auto &v : p)
    a.push_back({v.x, v.y, v.z});
  return a;
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

  std::vector<CityPlanCell> cells;
  for (uint64_t z = 0; z < 4; ++z)
    for (uint64_t x = 0; x < 5; ++x)
    {
      CityPlanCell c;
      c.id = z * 5 + x + 1;
      c.boundary = rect(x * 40, z * 40, 40, 40);
      c.outsideOldWall = x >= 2 || z >= 2;
      c.fertility = .15 + .15 * ((x + z) % 5);
      c.defense = .1 * x;
      cells.push_back(c);
    }
  cells[4].navigableShore = true;
  cells[9].floodRisk = .9;
  cells[14].floodRisk = .5;
  cells[19].slope = .8;

  CityTerritory territory;
  territory.revision = 2;
  territory.regions.districts = {{100, 1, DistrictUse::Residential, {1, 2, 6, 7}, {rect(0, 0, 80, 80)}, {}, {}, 6400, 0}};
  AgriculturalPatch farm;
  farm.id = 200;
  farm.boundary = rect(80, 0, 80, 40);
  territory.rural.patches = {farm};
  WorldSettlementSite site;
  site.brief.input.cells = cells;

  CitySkeleton skeleton;
  skeleton.revision = 2;
  skeleton.roadNodes = {
      {301, CityRoadNodeKind::Gate, {80, 0, 40}, {401}},
      {302, CityRoadNodeKind::Endpoint, {180, 0, 140}, {401}}};
  skeleton.roadEdges = {{401, 1, 301, 302, CityRoadKind::Approach, {{80, 0, 40}, {180, 0, 140}}, 2}};

  CityExteriorSettings settings;
  settings.maximumAccessDistance = 300;
  settings.cellUses = {
      {8, CityLandUse::RoadReserve},
      {11, CityLandUse::WallClearance},
      {12, CityLandUse::Orchard},
      {13, CityLandUse::ManagedWood},
      {16, CityLandUse::Cemetery},
      {17, CityLandUse::Quarry},
      {18, CityLandUse::FutureGrowth}};
  CityExteriorAllocation allocation;
  std::string error;
  check(allocateCityExterior(site, territory, skeleton, settings, allocation, &error), "allocate every world-derived planning cell: " + error);
  check(allocation.parcels.size() == cells.size(), "one authoritative parcel per city-domain planning cell");
  check(std::abs(allocation.allocatedArea - allocation.planningArea) < 1e-6, "allocated area exactly conserves the world-derived tessellation");
  check(allocation.anonymousArea == 0, "no anonymous exterior remainder");
  std::set<CityLandUse> uses;
  bool access = true;
  for (const auto &p : allocation.parcels)
  {
    uses.insert(p.use);
    if (p.access != CityParcelAccess::IntentionallyNone)
      access &= p.accessNode || p.access == CityParcelAccess::Easement;
  }
  check(uses.size() >= 12, "constraints, productive land, civic reserves and urban land remain visibly distinct");
  check(access, "every usable parcel has a road node or explicit easement");

  CitySkeleton augmented = skeleton;
  check(appendCityExterior(allocation, augmented, &error), "publish exterior parcels into city skeleton: " + error);
  check(augmented.regions.size() == cells.size(), "city skeleton owns only the city-domain parcel layer");
  check(!augmented.regionAccess.empty(), "parcel-to-road access is an explicit cross-graph relation");

  auto shuffled = cells;
  std::reverse(shuffled.begin(), shuffled.end());
  auto replaySite = site;
  replaySite.brief.input.cells = shuffled;
  CityExteriorAllocation replay;
  check(allocateCityExterior(replaySite, territory, skeleton, settings, replay, &error), "allocate permuted cells: " + error);
  std::vector<uint64_t> a, b;
  for (const auto &p : allocation.parcels)
    a.push_back(p.id);
  for (const auto &p : replay.parcels)
    b.push_back(p.id);
  check(a == b, "parcel identity is independent of input order");

  if (argc > 1)
  {
    const std::filesystem::path out = argv[1];
    std::filesystem::create_directories(out);
    json j;
    j["parcels"] = json::array();
    j["roads"] = json::array();
    for (const auto &p : allocation.parcels)
      j["parcels"].push_back({{"id", p.id}, {"cell", p.cell}, {"use", cityLandUseName(p.use)}, {"boundary", poly(p.boundary)}, {"reason", p.reason}, {"access", p.accessNode}});
    for (const auto &road : skeleton.roadEdges)
      j["roads"].push_back({{"path", poly(road.path)}, {"width", road.halfWidth}});
    std::ofstream(out / "city-exterior-c2.json") << j.dump(2);
  }
  std::cout << checks << " checks, " << failures << " failures\n";
  return failures ? 1 : 0;
}
