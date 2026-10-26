#include "json.hpp"
#include "procedural/world/MapPartition.hpp"
#include "procedural/world/WorldCityDemand.hpp"
#include "procedural/world/WorldCityDirectorAdapter.hpp"
#include "procedural/world/WorldCityInteriors.hpp"
#include "procedural/world/WorldSettlementDirector.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
using namespace procedural::world::rewrite;
using nlohmann::json;
using Poly = std::vector<Position>;
Poly rect(double x, double z, double w, double d)
{
  return {{x, 0, z}, {x + w, 0, z}, {x + w, 0, z + d}, {x, 0, z + d}};
}
json poly(const Poly &p)
{
  json j = json::array();
  for (auto v : p)
    j.push_back({v.x, v.y, v.z});
  return j;
}
json encode(const DwellingPlan &p)
{
  json j = {{"id", p.id}, {"use", int(p.use)}, {"residents", p.residentCapacity}, {"workers", p.workerCapacity}};
  j["rooms"] = json::array();
  j["walls"] = json::array();
  j["doors"] = json::array();
  j["floors"] = json::array();
  j["stairs"] = json::array();
  for (auto &r : p.rooms)
    j["rooms"].push_back({{"id", r.id}, {"floor", r.floor}, {"use", int(r.use)}, {"boundary", poly(r.boundary)}, {"area", r.usableArea}});
  for (auto &w : p.walls)
    j["walls"].push_back({{"id", w.id}, {"floor", w.floor}, {"path", poly({w.a, w.b})}, {"thickness", w.thickness}, {"exterior", w.exterior}});
  for (auto &d : p.doors)
    j["doors"].push_back({{"wall", d.wall}, {"room", d.room}, {"floor", d.floor}, {"center", poly({d.center})}, {"width", d.width}, {"private", d.privateAccess}, {"exterior", d.exterior}});
  for (auto &f : p.floors)
  {
    json c = json::array(), s = json::array();
    for (auto &q : f.circulation)
      c.push_back(poly(q));
    for (auto &q : f.slabs)
      s.push_back(poly(q));
    j["floors"].push_back(
        {{"elevation", f.elevation},
         {"height", f.height},
         {"gross", f.grossArea},
         {"roomArea", f.roomArea},
         {"circulationArea", f.circulationArea},
         {"wallArea", f.wallArea},
         {"coreArea", f.coreArea},
         {"circulation", c},
         {"slabs", s}});
  }
  for (auto &s : p.stairs)
    j["stairs"].push_back({{"opening", poly(s.opening)}, {"lower", poly(s.lowerLanding)}, {"upper", poly(s.upperLanding)}, {"risers", s.risers}, {"rise", s.rise}, {"tread", s.tread}, {"private", s.privateAccess}});
  return j;
}
json encode(const SettlementState &s)
{
  json j = {
    {"id", s.settlement},
    {"revision", s.revision},
    {"year", s.year},
    {"population", s.population},
    {"housed", s.housed},
    {"unhoused", s.unhoused},
    {"workers", s.workers},
    {"employed", s.employed},
    {"unserved", s.unserved},
    {"foodShortfall", s.foodShortfall},
    {"waterShortfall", s.waterShortfall}};
  j["sizeClass"] = unsigned(s.sizeClass);
  j["fitAttempts"] = s.fitAttempts;
  j["serviceReserves"] = s.serviceReserveSites;
  j["rejections"] = s.rejections;
  for (auto key : {"households", "buildings", "tenancies", "employment", "services", "history", "unmet"})
    j[key] = json::array();
  for (auto h : s.households)
    j["households"].push_back({{"id", h.id}, {"members", h.members}, {"workers", h.workers}, {"collective", h.collective}});
  for (auto &b : s.buildings)
    j["buildings"].push_back(
        {{"id", b.id}, {"site", b.site}, {"node", b.node}, {"programme", b.programme}, {"kind", int(b.kind)}, {"builtYear", b.builtYear}, {"serviceCapacity", b.serviceCapacity}, {"interior", encode(b.interior)}});
  for (auto t : s.tenancies)
    j["tenancies"].push_back({{"household", t.household}, {"building", t.building}, {"residents", t.residents}});
  for (auto e : s.employment)
    j["employment"].push_back({{"household", e.household}, {"building", e.building}, {"workers", e.workers}, {"travel", e.travelCost}});
  for (auto e : s.services)
    j["services"].push_back({{"household", e.household}, {"building", e.building}, {"residents", e.residents}, {"travel", e.travelCost}});
  for (auto e : s.history)
    j["history"].push_back({{"id", e.id}, {"year", e.year}, {"arrivals", e.arrivals}, {"departures", e.departures}, {"built", e.buildingsAdded}});
  for (auto d : s.unmetProgrammes)
    j["unmet"].push_back({{"programme", d.programme}, {"count", d.count}});
  return j;
}
int main(int argc, char **argv)
{
  int checks = 0, failures = 0;
  auto check = [&](bool b, std::string s)
  {
    ++checks;
    failures += !b;
    std::cout << (b ? "PASS " : "FAIL ") << s << std::endl;
  };
  std::string error;
  std::filesystem::path output = argc > 1 ? argv[1] : "";
  if (!output.empty())
    std::filesystem::create_directories(output);
  for (int kind = 0; kind < 6; ++kind)
  {
    DwellingBrief b;
    b.id = kind + 1;
    b.use = kind == 5 ? DwellingUse::Cottage : DwellingUse(kind);
    b.shell = rect(0, 0, kind == 2 ? 18 : 10, kind == 2 ? 24 : 16);
    b.entrance = {kind == 2 ? 9. : 5., 0, 0};
    b.floors = kind == 1 ? 2 : 1;
    b.availableHeight = 6;
    if (kind == 5)
    {
      b.shell = rect(0, 0, 4, 10);
      b.entrance = {2, 0, 0};
    }
    DwellingPlan p;
    bool ok = generateDwellingPlan(b, p, &error);
    check(ok, "dwelling programme " + std::to_string(kind) + ": " + error);
    if (!ok)
      continue;
    bool ledger = true, doors = true;
    for (auto f : p.floors)
      ledger &= std::abs(f.grossArea - f.roomArea - f.circulationArea - f.coreArea - f.wallArea) < 1e-7;
    for (auto r : p.rooms)
      doors &= std::count_if(
                   p.doors.begin(),
                   p.doors.end(),
                   [&](auto d)
                   {
                     return d.room == r.id && !d.shopOpening; // Selling openings are not access doors.
                   }) == 1;
    check(ledger && doors, "room access and floor area ledger");
    std::vector<DwellingTriangle> mesh;
    check(meshDwellingPlan(p, mesh, &error) && !mesh.empty(), "walls with openings, floors and stair solids mesh: " + error);
    if (kind == 1)
    {
      // A straight flight or a dog-leg pair: private flights whose rises add up to the floor height.
      double rise = 0;
      bool privateStack = !p.stairs.empty();
      for (const auto &s : p.stairs)
      {
        rise += s.rise;
        privateStack &= s.privateAccess && s.lowerFloor == 0;
      }
      check(privateStack && p.floors.size() == 2 && std::abs(rise - (p.floors[1].elevation - p.floors[0].elevation)) < 1e-6, "shop-house reserves one connected floor-stack stair");
      bool stack = true;
      for (auto r : p.rooms)
        if (r.use == RoomUse::Sleeping)
          stack &= r.floor == 1;
      check(stack && p.residentCapacity > 0 && p.workerCapacity > 0, "shop below, household above, separate capacities");
    }
    if (kind == 2)
      check(p.residentCapacity >= 24, "large worker lodging has measured sleeping capacity");
    if (!output.empty())
    {
      auto j = encode(p);
      j["shell"] = poly(b.shell);
      j["triangles"] = json::array();
      for (auto t : mesh)
        j["triangles"].push_back({{"points", poly({t.a, t.b, t.c})}, {"material", t.material}});
      std::ofstream(output / ("dwelling-" + std::to_string(kind) + ".json")) << j.dump(2);
    }
    auto bad = b;
    bad.entrance.x = 0.1;
    DwellingPlan sentinel;
    sentinel.id = 999;
    check(!generateDwellingPlan(bad, sentinel, &error) && sentinel.id == 999, "infeasible fixed door rejects atomically");
    bad = b;
    bad.shell[2].x += 1;
    check(!generateDwellingPlan(bad, sentinel, &error), "unsupported skew shell explicitly rejected");
  }
  bool variants = true;
  for (uint64_t seed = 1; seed <= 24; ++seed)
  {
    DwellingBrief b;
    b.id = 100 + seed;
    b.seed = seed;
    b.use = DwellingUse::ShopHouse;
    b.floors = 2;
    b.floorRise = 2.4 + .1 * (seed % 8);
    b.availableHeight = 6.4;
    b.foundation = seed * .25;
    auto transform = [&](Position v)
    {
      double angle = seed * .37;
      return Position{50 + v.x * std::cos(angle) - v.z * std::sin(angle), 0, -30 + v.x * std::sin(angle) + v.z * std::cos(angle)};
    };
    b.shell = rect(0, 0, 10, 16);
    for (auto &v : b.shell)
      v = transform(v);
    b.entrance = transform({5, 0, 0});
    DwellingPlan p;
    std::vector<DwellingTriangle> mesh;
    variants &= generateDwellingPlan(b, p, &error) && meshDwellingPlan(p, mesh, &error);
  }
  check(variants, "24 seeds, rotations, foundation elevations and floor rises qualify the complete dwelling stack");
  std::vector<SettlementHousehold> population;
  check(generateSettlementPopulation({7, 731, 731}, population, &error), "population range selection");
  uint32_t total = 0;
  for (auto h : population)
    total += h.members;
  check(total == 731, "exact household population conservation");
  auto sentinel = population;
  check(!generateSettlementPopulation({7, 3, 2}, sentinel, &error) && sentinel.size() == population.size(), "invalid population range is atomic");
  SettlementDirectorInput input;
  input.settlement = 77;
  input.seed = 19;
  input.year = 100;
  input.population = {7, 180, 180};
  input.catalogue = settlementProgrammeCatalogue();
  for (uint32_t i = 0; i < 120; ++i)
  {
    double x = (i % 20) * 24, z = (i / 20) * 36;
    input.nodes.push_back({i + 1, {x, 0, z}});
    if (i % 20)
      input.edges.push_back({i, i + 1, 24});
    if (i >= 20)
      input.edges.push_back({i - 19, i + 1, 36});
    SettlementSite s;
    s.id = 1000 + i;
    s.node = i + 1;
    s.centrality = 1 - double(i) / 120;
    s.shell.id = s.id;
    s.shell.shell = rect(x, z, i % 10 == 0 ? 18 : 10, i % 10 == 0 ? 24 : 16);
    s.shell.entrance = {x + (i % 10 == 0 ? 9. : 5.), 0, z};
    s.shell.availableHeight = 6;
    input.sites.push_back(s);
  }
  SettlementState base;
  bool ok = planSettlement(input, base, &error);
  check(ok, "programme director: " + error);
  if (!ok)
    return 1;
  check(base.population == base.housed + base.unhoused && base.housed > 140, "whole-household capacity accounting");
  check(base.employed <= base.workers, "employment cannot duplicate available workers");
  auto otherCity = input;
  otherCity.settlement = 78;
  SettlementState other;
  bool distinct = planSettlement(otherCity, other, &error);
  std::set<uint64_t> homeIds;
  for (auto h : base.households)
    homeIds.insert(h.id);
  for (auto h : other.households)
    distinct &= !homeIds.count(h.id);
  check(distinct, "household identity is namespaced by settlement");
  auto reversed = input;
  std::reverse(reversed.sites.begin(), reversed.sites.end());
  std::reverse(reversed.nodes.begin(), reversed.nodes.end());
  std::reverse(reversed.edges.begin(), reversed.edges.end());
  std::reverse(reversed.catalogue.begin(), reversed.catalogue.end());
  SettlementState repeat;
  check(planSettlement(reversed, repeat, &error) && encode(base) == encode(repeat), "site/network/catalogue permutation determinism");
  auto blocked = input;
  blocked.edges.clear();
  blocked.maximumServiceTravel = 0;
  SettlementState disconnected;
  check(planSettlement(blocked, disconnected, &error) && disconnected.unserved > 0, "nearby services across disconnected roads cannot serve households");
  auto scarce = input;
  scarce.maximumNewBuildings = 2;
  scarce.foodResidentCapacity = 10;
  scarce.waterResidentCapacity = 8;
  SettlementState shortfall;
  check(
      planSettlement(scarce, shortfall, &error) && shortfall.housed <= 8 && shortfall.foodShortfall == 170 && shortfall.waterShortfall == 172 && shortfall.unhoused > 0,
      "limited sites and supplies retain explicit deficits");
  SettlementGrowth grow;
  grow.event = 2;
  grow.expectedRevision = base.revision;
  grow.year = 110;
  grow.arrivals = {5, 120, 120};
  SettlementState expanded;
  check(advanceSettlement(input, base, grow, expanded, &error), "growth transaction: " + error);
  check(expanded.population == 300 && expanded.revision == 2 && expanded.buildings.size() >= base.buildings.size(), "growth preserves population and adds prepared sites");
  bool retained = true;
  for (auto &b : base.buildings)
  {
    auto it = std::find_if(
        expanded.buildings.begin(),
        expanded.buildings.end(),
        [&](auto &a)
        {
          return a.id == b.id;
        });
    retained &= it != expanded.buildings.end() && encode(it->interior) == encode(b.interior) && it->builtYear == b.builtYear;
  }
  for (auto t : base.tenancies)
    retained &= std::any_of(
        expanded.tenancies.begin(),
        expanded.tenancies.end(),
        [&](auto a)
        {
          return a.household == t.household && a.building == t.building;
        });
  check(retained, "growth keeps accepted interiors and occupied homes unchanged");
  SettlementState replay;
  check(advanceSettlement(input, base, grow, replay, &error) && encode(replay) == encode(expanded), "event replay is deterministic");
  SettlementState unchanged = expanded;
  check(!advanceSettlement(input, expanded, grow, unchanged, &error) && encode(unchanged) == encode(expanded), "stale/repeated event rejects atomically");
  SettlementGrowth decline;
  decline.event = 3;
  decline.expectedRevision = 2;
  decline.year = 120;
  for (size_t i = 0; i < 10; ++i)
    decline.departingHouseholds.push_back(expanded.households[i].id);
  SettlementState reduced;
  check(
      advanceSettlement(input, expanded, decline, reduced, &error) && reduced.population < expanded.population && reduced.buildings.size() == expanded.buildings.size(),
      "decline vacates units without deleting old fabric");
  if (!output.empty())
  {
    std::ofstream(output / "settlement-100.json") << encode(base).dump(2);
    std::ofstream(output / "settlement-110.json") << encode(expanded).dump(2);
    std::ofstream(output / "settlement-120.json") << encode(reduced).dump(2);
  }
  // Larger demand, short catchments and terrain locks exercise service repair.
  auto cityInput = input;
  cityInput.population = {11, 900, 900};
  cityInput.maximumServiceTravel = 230;
  for (uint32_t i = 120; i < 400; ++i)
  {
    double x = (i % 20) * 24, z = (i / 20) * 36;
    cityInput.nodes.push_back({i + 1, {x, 0, z}});
    if (i % 20)
      cityInput.edges.push_back({i, i + 1, 24});
    cityInput.edges.push_back({i - 19, i + 1, 36});
    auto site = input.sites[i % 120];
    site.id = 1000 + i;
    site.node = i + 1;
    site.shell.shell = rect(x, z, i % 10 == 0 ? 18 : 10, i % 10 == 0 ? 24 : 16);
    site.shell.entrance = {x + (i % 10 == 0 ? 9. : 5.), 0, z};
    cityInput.sites.push_back(site);
  }
  SettlementState cityState;
  check(planSettlement(cityInput, cityState, &error) && cityState.housed > 850, "city-scale population derives programme demand and room capacities");
  size_t temples = std::count_if(
      cityState.buildings.begin(),
      cityState.buildings.end(),
      [](auto &b)
      {
        return b.kind == ProgrammeKind::Temple;
      });
  check(base.sizeClass == SettlementSizeClass::Village && cityState.sizeClass == SettlementSizeClass::Town, "population size labels stay separate from political status and geometry");
  check(temples >= 3 && cityState.unserved == 0, "repeated temples cover finite-capacity short walking catchments");
  auto locked = input;
  for (size_t i = 0; i < locked.sites.size(); i += 2)
    locked.sites[i].slope = .5;
  for (size_t i = 1; i < locked.sites.size(); i += 4)
    locked.sites[i].water = false;
  SettlementState geography;
  bool geo = planSettlement(locked, geography, &error);
  for (auto &b : geography.buildings)
  {
    auto i = b.site - 1000;
    geo &= locked.sites[i].slope <= .12 && locked.sites[i].water;
  }
  check(geo, "hard terrain and water exclusions cannot be outweighed by site scoring");
  auto capped = input;
  capped.maximumFitAttempts = 0;
  SettlementState noSearch;
  check(planSettlement(capped, noSearch, &error) && noSearch.buildings.empty() && noSearch.unhoused == 180, "exhausted fit budget reports deficits without unsafe fallback");
  SettlementGrowth refill;
  refill.event = 4;
  refill.expectedRevision = reduced.revision;
  refill.year = 130;
  refill.arrivals = {1, 15, 15};
  auto vacantInput = input;
  vacantInput.maximumNewBuildings = 0;
  SettlementState refilled;
  check(
      advanceSettlement(vacantInput, reduced, refill, refilled, &error) && refilled.buildings.size() == reduced.buildings.size() && refilled.housed > reduced.housed,
      "growth fills compatible vacancies before any construction");
  auto changed = input;
  for (auto &p : changed.catalogue)
    if (p.kind == ProgrammeKind::Home)
      p.floors = 2;
  SettlementState same = reduced;
  check(!advanceSettlement(changed, reduced, refill, same, &error) && encode(same) == encode(reduced), "programme changes require an explicit conversion instead of rewriting old interiors");
  if (!output.empty())
  {
    std::ofstream(output / "city-scale.json") << encode(cityState).dump(2);
    json networks;
    for (auto pair : std::vector<std::pair<std::string, const SettlementDirectorInput *>>{{"village", &input}, {"city", &cityInput}})
    {
      auto in = pair.second;
      json n;
      n["nodes"] = json::array();
      n["edges"] = json::array();
      for (auto node : in->nodes)
        n["nodes"].push_back({{"id", node.id}, {"position", poly({node.position})}});
      for (auto edge : in->edges)
        n["edges"].push_back({edge.a, edge.b, edge.cost});
      n["serviceLimit"] = in->maximumServiceTravel;
      n["commuteLimit"] = in->maximumCommute;
      networks[pair.first] = n;
    }
    std::ofstream(output / "travel-networks.json") << networks.dump(2);
  }
  {
    json catalogue;
    std::ifstream(CITY_PROGRAMME_CATALOGUE_PATH) >> catalogue;
    std::set<std::string> ids;
    bool coherent = true;
    for (auto p : catalogue["programs"])
      coherent &= ids.insert(p["id"].get<std::string>()).second;
    for (auto p : input.catalogue)
    {
      auto found = std::find_if(
          catalogue["programs"].begin(),
          catalogue["programs"].end(),
          [&](auto j)
          {
            return j["id"] == p.id;
          });
      coherent &= found != catalogue["programs"].end();
      if (found != catalogue["programs"].end())
        coherent &= (*found)["runtimePrototype"]["floors"] == p.floors && (*found)["runtimePrototype"]["minimumPlotArea"] == p.minimumPlotArea;
    }
    check(coherent && ids.size() == 92, "six qualified runtime programmes retain the researched 92-catalogue identities");
  }
  CityPopulationDemand landDemand;
  CityDemandSettings landSettings;
  landSettings.planningCellAreaAllowance = 225;
  check(
      deriveCityPopulationDemand(population, landSettings, landDemand, &error) && landDemand.population == 731 && landDemand.circulationArea > 0,
      "population derives district and programme land requests before geometry");
  std::vector<CityPlanCell> land;
  for (int z = 0; z < 20; ++z)
    for (int x = 0; x < 20; ++x)
    {
      CityPlanCell c;
      c.id = 1 + z * 20 + x;
      c.boundary = rect(x * 15, z * 15, 15, 15);
      c.secure = true;
      c.allocatable = x % 5 != 0 && z % 5 != 0; // Retained circulation crosses the district graph.
      land.push_back(c);
    }
  CityMasterPlan allocated;
  bool allocatedOK = generateCityMasterPlan(land, landDemand.districts, {1, 400}, {}, allocated, &error);
  check(allocatedOK && allocated.districts.size() == landDemand.districts.size(), "population demand feeds existing connected city-region allocator: " + error);
  SettlementFoodSupply food;
  std::vector<SettlementFoodHolding> holdings = {{1, 10000, .5, .2, 2, 1, true}, {2, 20000, .5, .2, 2, 4, false}};
  check(calculateSettlementFoodSupply(holdings, 100, food, &error) && food.supportedResidents == 20 && food.annualUsableYield == 2000, "food supply accounts for crop area, losses, labour and reachability");
  auto unchangedFood = food;
  holdings.push_back(holdings.front());
  check(!calculateSettlementFoodSupply(holdings, 100, food, &error) && food.annualUsableYield == unchangedFood.annualUsableYield, "duplicate holding cannot inflate food capacity");
  if (argc > 2)
  {
    json source;
    std::ifstream(argv[2]) >> source;
    auto decode = [](json ring)
    {
      Poly p;
      for (auto v : ring)
        p.push_back({v[0], v[1], v[2]});
      return p;
    };
    CityLayout city;
    city.revision = source.value("revision", uint64_t(1));
    std::vector<CityInteriorRequest> requests;
    for (auto j : source["objects"])
    {
      CityObject o;
      o.id = j["id"];
      o.street = j["street"];
      o.footprint = decode(j["footprint"]);
      auto roadPoint = j["access"];
      o.access = {roadPoint[0], roadPoint[1], roadPoint[2]};
      o.kind = CityObjectKind(j["kind"].get<int>());
      o.buildingType = CityBuildingType(j["buildingType"].get<int>());
      o.foundation = j["foundation"];
      auto v = j["entrance"];
      o.entrance = {v[0], v[1], v[2]};
      o.courtyard = decode(j["courtyard"]);
      for (auto part : j["parts"])
        o.parts.push_back({decode(part["footprint"]), part["height"]});
      city.objects.push_back(o);
      if (o.buildingType == CityBuildingType::Cottage)
        requests.push_back({o.id, DwellingUse::Cottage, 1});
      if (o.buildingType == CityBuildingType::ShopHouse)
        requests.push_back({o.id, DwellingUse::ShopHouse, 2});
      if (o.buildingType == CityBuildingType::Warehouse)
        requests.push_back({o.id, DwellingUse::Warehouse, 1});
      if (o.buildingType == CityBuildingType::WorkerLodging)
        requests.push_back({o.id, DwellingUse::Lodging, 1});
    }
    for (auto j : source["streets"])
    {
      CityStreet street;
      street.id = j["id"];
      street.path = decode(j["path"]);
      street.a = street.path.front();
      street.b = street.path.back();
      street.halfWidth = j["width"].get<double>() * .5;
      city.streets.push_back(street);
    }
    for (auto j : source["walls"])
    {
      auto line = decode(j[0]);
      CityStreet wall;
      wall.a = line[0];
      wall.b = line[1];
      wall.halfWidth = j[1];
      city.walls.push_back(wall);
    }
    for (auto j : source["towers"])
    {
      CityWallTower t;
      t.id = j["id"];
      t.footprint = decode(j["footprint"]);
      city.towers.push_back(t);
    }
    auto capitalPolicy = input;
    capitalPolicy.settlement = 88;
    capitalPolicy.population = {19, 300, 300};
    capitalPolicy.sites.clear();
    // Explicit synthetic fixture eligibility. Production must provide actual
    // terrain/water predicates instead of inheriting these authored flags.
    for (auto &o : city.objects)
    {
      SettlementSite s;
      s.id = o.id;
      s.buildable = true;
      s.water = true;
      s.slope = 0;
      capitalPolicy.sites.push_back(s);
    }
    SettlementDirectorInput capitalInput;
    bool adapted = prepareCityDirector(city, capitalPolicy, capitalInput, &error);
    check(adapted && capitalInput.sites.size() > 900, "accepted city paths and fixed house entrances feed the population director: " + error);
    SettlementState capitalPopulation;
    bool populated = adapted && planSettlement(capitalInput, capitalPopulation, &error);
    check(
        populated && capitalPopulation.housed > 0 && capitalPopulation.unhoused > 0 && capitalPopulation.housed + capitalPopulation.unhoused == 300,
        "fixed capital shells admit whole households and report residual demand: " + error);
    auto missingPolicy = capitalPolicy;
    missingPolicy.sites.clear();
    SettlementDirectorInput missingInput;
    check(prepareCityDirector(city, missingPolicy, missingInput, &error) && missingInput.sites.empty(), "city adapter requires explicit terrain and water eligibility");
    if (populated && !output.empty())
    {
      json report = encode(capitalPopulation);
      report["source"] = source;
      for (size_t k = 0; k < capitalPopulation.buildings.size(); ++k)
      {
        const auto &building = capitalPopulation.buildings[k];
        auto site = std::find_if(capitalInput.sites.begin(), capitalInput.sites.end(), [&](auto &candidate) { return candidate.id == building.site; });
        report["buildings"][k]["interior"]["shell"] = poly(site->shell.shell);
      }
      report["serviceLimit"] = capitalInput.maximumServiceTravel;
      report["commuteLimit"] = capitalInput.maximumCommute;
      report["nodes"] = json::array();
      report["edges"] = json::array();
      for (auto n : capitalInput.nodes)
        report["nodes"].push_back({{"id", n.id}, {"position", poly({n.position})}});
      for (auto e : capitalInput.edges)
        report["edges"].push_back({e.a, e.b, e.cost});
      std::ofstream(output / "capital-population.json") << report.dump(2);
    }
    CityInteriors admitted;
    check(compileCityInteriors(city, requests, 7, admitted, &error) && admitted.dwellings.size() > 10, "accepted capital shells admit interiors without moving walls/doors: " + error);
    bool meshed = true;
    for (auto &p : admitted.dwellings)
    {
      std::vector<DwellingTriangle> triangles;
      meshed &= meshDwellingPlan(p, triangles, &error);
    }
    check(meshed, "admitted capital interiors produce structural meshes");
    check(admitted.dwellings.size() + admitted.unresolved.size() == requests.size(), "every city request is solved or explicitly unresolved");
    json report;
    report["requested"] = requests.size();
    report["solved"] = json::array();
    report["unresolved"] = json::array();
    for (auto &p : admitted.dwellings)
      report["solved"].push_back(encode(p));
    for (auto &r : admitted.unresolved)
      report["unresolved"].push_back({{"object", r.object}, {"reason", r.reason}});
    if (!output.empty())
      std::ofstream(output / "capital-interiors.json") << report.dump(2);
  }
  std::cout << "DIRECTOR_TESTS assertions=" << checks << " failures=" << failures << " population=" << expanded.population << " housed=" << expanded.housed << " buildings=" << expanded.buildings.size() << std::endl;
  return failures ? 1 : 0;
}
