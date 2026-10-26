#include "json.hpp"
#include "procedural/world/WorldCityMasterPlan.hpp"
#include "procedural/world/WorldSurfaceArrangement.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
using namespace procedural::world::rewrite;
using nlohmann::json;
std::vector<Position> rect(double x, double z, double w = 20, double h = 20)
{
  return {{x, 0, z}, {x + w, 0, z}, {x + w, 0, z + h}, {x, 0, z + h}};
}
json poly(const std::vector<Position> &p)
{
  json a = json::array();
  for (auto v : p)
    a.push_back({v.x, v.z});
  return a;
}
json encode(const CityMasterPlan &p)
{
  json j;
  j["revision"] = p.revision;
  j["inputFingerprint"] = p.inputFingerprint;
  j["gateConnections"] = json::array();
  for (const auto &route : p.gateConnections)
    j["gateConnections"].push_back(poly(route));
  j["districts"] = json::array();
  j["omissions"] = json::array();
  j["unassigned"] = p.unassignedCells;
  j["assignedArea"] = p.assignedArea;
  j["unassignedArea"] = p.unassignedArea;
  j["excludedArea"] = p.excludedArea;
  for (auto &d : p.districts)
  {
    json pieces = json::array();
    for (auto &q : d.pieces)
      pieces.push_back(poly(q));
    j["districts"].push_back(
        {{"id", d.id},
         {"use", districtUseName(d.use)},
         {"anchor", d.anchorCell},
         {"cells", d.cells},
         {"pieces", pieces},
         {"access", poly(d.access)},
         {"accessCells", d.accessCells},
         {"area", d.area},
         {"unmetTarget", d.unmetTargetArea}});
  }
  for (auto &o : p.omissions)
    j["omissions"].push_back({{"request", o.request}, {"reason", o.reason}});
  return j;
}
std::vector<CityPlanCell> fixture(uint64_t seed, bool port)
{
  Position vertices[8][10];
  for (int z = 0; z < 8; ++z)
    for (int x = 0; x < 10; ++x)
    {
      double dx = (x && x < 9) ? double(featureSeed(seed, 1, z * 10 + x, 1) % 5000) / 1000. - 2.5 : 0;
      double dz = (z && z < 7) ? double(featureSeed(seed, 1, z * 10 + x, 2) % 5000) / 1000. - 2.5 : 0;
      vertices[z][x] = {x * 20. + dx, 0, z * 20. + dz};
    }
  std::vector<CityPlanCell> cells;
  for (int z = 0; z < 7; ++z)
    for (int x = 0; x < 9; ++x)
    {
      CityPlanCell c;
      c.id = z * 9 + x + 1;
      c.boundary = {vertices[z][x], vertices[z][x + 1], vertices[z + 1][x + 1], vertices[z + 1][x]};
      c.defense = z >= 4 && x <= 3 ? .9 : .2;
      c.secure = z >= 3 && x <= 4;
      c.navigableShore = port && x == 8;
      c.outsideOldWall = z <= 1;
      c.fertility = x <= 3 ? .8 : .3;
      c.buildable = !(x == 5 && z == 3);
      c.floodRisk = x == 7 && z == 3 ? .8 : 0;
      cells.push_back(c);
    }
  return cells;
}
int main(int argc, char **argv)
{
  int checks = 0, failures = 0;
  auto check = [&](bool b, const std::string &n)
  {
    ++checks;
    failures += !b;
    std::cout << (b ? "PASS " : "FAIL ") << n << '\n';
  };
  std::filesystem::path output = argc > 1 ? argv[1] : "";
  if (!output.empty())
    std::filesystem::create_directories(output);
  for (int scenario = 0; scenario < 3; ++scenario)
    for (uint64_t seed : {7, 19, 103})
    {
      bool port = scenario == 1;
      auto cells = fixture(seed, port);
      CityPlanSettings settings;
      settings.seed = seed;
      std::vector<DistrictRequest> requests{
        {1, DistrictUse::Castle, 650, 1800, true},
        {2, DistrictUse::Merchant, 650, 2500, true},
        {3, DistrictUse::Residential, 650, 3000, true},
        {4, DistrictUse::Suburb, 650, 2600, true},
        {5, DistrictUse::Harbor, 350, 1800, port},
        {6, DistrictUse::Military, 650, 1800, true},
        {7, DistrictUse::Prison, 350, 950, true},
        {8, DistrictUse::Manor, 350, 1300, true}};
      if (scenario == 2)
      {
        requests = {
          {1, DistrictUse::Manor, 650, 2400, true},
          {2, DistrictUse::Merchant, 350, 1500, true},
          {3, DistrictUse::Residential, 650, 2800, true},
          {4, DistrictUse::Suburb, 650, 2200, true},
          {5, DistrictUse::Harbor, 350, 1200, false}};
      }
      CityMasterPlan p, q;
      std::string error;
      std::string tag = std::to_string(scenario) + "-" + std::to_string(seed);
      bool ok = generateCityMasterPlan(cells, requests, {1, 36}, settings, p, &error);
      check(ok, tag + " allocate " + error);
      if (!ok)
        continue;
      auto reversed = cells;
      std::reverse(reversed.begin(), reversed.end());
      std::reverse(requests.begin(), requests.end());
      check(generateCityMasterPlan(reversed, requests, {36, 1}, settings, q, &error) && encode(p) == encode(q), tag + " input-order determinism");
      std::set<uint64_t> used;
      bool unique = true, eligible = true, access = true, minimum = true;
      for (auto &d : p.districts)
      {
        auto request = *std::find_if(
            requests.begin(),
            requests.end(),
            [&](auto &r)
            {
              return r.id == d.id;
            });
        minimum &= d.area + 1e-7 >= request.minimumArea && d.area <= request.targetArea + 1e-7;
        access &= !d.accessCells.empty() && (d.accessCells.front() == 1) && d.accessCells.back() == d.anchorCell;
        for (auto id : d.cells)
        {
          unique &= used.insert(id).second;
          auto &c = cells[id - 1];
          eligible &= c.buildable && c.floodRisk <= .2;
          if (d.use == DistrictUse::Harbor)
            eligible &= c.navigableShore;
          if (d.use == DistrictUse::Castle)
            eligible &= c.defense >= .6;
          if (d.use == DistrictUse::Military || d.use == DistrictUse::Prison)
            eligible &= c.secure;
          if (d.use == DistrictUse::Suburb)
            eligible &= c.outsideOldWall;
          if (d.use == DistrictUse::Manor)
            eligible &= c.fertility >= .5;
        }
        // Independent grid topology check: fixture neighbors share full jittered sides.
        std::set<uint64_t> reached{d.anchorCell};
        bool changed = true;
        while (changed)
        {
          changed = false;
          for (auto a : d.cells)
            if (!reached.count(a))
              for (auto b : reached)
                if ((a - 1) / 9 == (b - 1) / 9 && std::abs(int(a) - int(b)) == 1 || std::abs(int(a) - int(b)) == 9)
                {
                  reached.insert(a);
                  changed = true;
                  break;
                }
        }
        check(reached.size() == d.cells.size(), tag + " connected " + districtUseName(d.use));
        for (size_t k = 1; k < d.accessCells.size(); ++k)
        {
          auto a = d.accessCells[k - 1], b = d.accessCells[k];
          access &= ((a - 1) / 9 == (b - 1) / 9 && std::abs(int(a) - int(b)) == 1) || std::abs(int(a) - int(b)) == 9;
          access &= cells[b - 1].buildable && cells[b - 1].floodRisk <= .2;
        }
      }
      check(unique && eligible, tag + " disjoint hard eligibility");
      check(access, tag + " gate paths avoid excluded cells");
      check(minimum, tag + " minimum and maximum area budgets");
      check(std::abs(p.assignedArea + p.unassignedArea + p.excludedArea - 25200) < 1e-6, tag + " complete land accounting");
      check(port ? p.omissions.empty() : p.omissions.size() == 1, tag + " inland harbor explicitly omitted");
      check(!cityLandmarkReservations(p).empty(), tag + " compound reservation adapter");
      // Feed the real polygon pieces to the authoritative contour/CDT pipeline.
      std::vector<SurfaceContour> contours;
      uint64_t cid = 1;
      for (auto &d : p.districts)
        for (auto &piece : d.pieces)
        {
          SurfaceContour c;
          c.id = cid++;
          c.loop = piece;
          c.material = 20 + unsigned(d.use);
          c.terrainSurface = true;
          contours.push_back(c);
        }
      SurfaceMapSettings ms;
      ms.maxX = 180;
      ms.maxZ = 140;
      SurfaceArrangement map;
      SurfaceArrangementMesh mesh;
      auto terrain = [](double, double)
      {
        return 0.;
      };
      check(arrangeSurfaceContours(contours, ms, terrain, seed, map, &error), tag + " district contours " + error);
      check(meshSurfaceArrangement(map, terrain, {}, mesh, &error) && std::abs(mesh.groundArea - 25200) < 1e-5, tag + " preserved district CDT coverage " + error);
      std::map<uint32_t, double> expectedArea, actualArea;
      for (const auto &district : p.districts)
        expectedArea[20 + unsigned(district.use)] += district.area;
      for (const auto &triangle : mesh.triangles)
      {
        auto a = mesh.vertices[triangle.vertices[0]], b = mesh.vertices[triangle.vertices[1]], c = mesh.vertices[triangle.vertices[2]];
        actualArea[triangle.material] += std::abs((b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x)) * .5;
      }
      bool materialAreas = true;
      for (auto [material, expected] : expectedArea)
        materialAreas &= std::abs(actualArea[material] - expected) < 1e-5;
      check(materialAreas, tag + " CDT preserves each semantic district area");
      if (!output.empty())
      {
        auto j = encode(p);
        j["cells"] = json::array();
        for (auto &c : cells)
          j["cells"].push_back({{"id", c.id}, {"boundary", poly(c.boundary)}, {"excluded", !c.buildable || c.floodRisk > .2}, {"navigable", c.navigableShore}});
        j["seed"] = seed;
        j["scenario"] = scenario;
        j["cdtTriangles"] = mesh.triangles.size();
        std::ofstream(output / (tag + ".json")) << j.dump(2);
      }
      std::cout << "CITY_PLAN_METRIC fixture=" << tag << " districts=" << p.districts.size() << " optional_omissions=" << p.omissions.size() << " assigned_area=" << p.assignedArea
                << " cdt_triangles=" << mesh.triangles.size() << '\n';
    }
  CityPlanSettings settings;
  CityMasterPlan sentinel;
  sentinel.revision = 777;
  std::string error;
  auto reject = [&](std::vector<CityPlanCell> c, std::vector<DistrictRequest> r, std::vector<uint64_t> g, const std::string &name)
  {
    auto p = sentinel;
    check(!generateCityMasterPlan(c, r, g, settings, p, &error) && encode(p) == encode(sentinel) && !error.empty(), name + " fails atomically");
  };
  CityPlanCell a;
  a.id = 1;
  a.boundary = rect(0, 0);
  CityPlanCell b = a;
  b.id = 2;
  b.boundary = rect(20, 20);
  DistrictRequest home{1, DistrictUse::Residential, 100, 1000, true};
  {
    auto summit=a, bailey=a; summit.defense=.9;
    bailey.id=2;bailey.defense=.4;bailey.boundary=rect(20,0);
    DistrictRequest castle{1,DistrictUse::Castle,600,900,true};
    castle.preferredCell=1;castle.allowLowerTerraces=true;
    CityMasterPlan p;
    check(generateCityMasterPlan({summit,bailey},{castle},{1},settings,p,&error) &&
          p.districts.size()==1 && p.districts.front().anchorCell==1 && p.districts.front().cells.size()==2,
          "historical castle keeps the summit anchor and expands a connected lower bailey");
    castle.allowLowerTerraces=false;
    reject({summit,bailey},{castle},{1},"strict castle still rejects insufficient defensive land");
    auto civic=summit;civic.id=2;civic.defense=.5;civic.boundary=rect(20,0);
    auto lower=bailey;lower.id=3;lower.boundary=rect(40,0);
    auto outer=lower;outer.id=4;outer.boundary=rect(60,0);
    DistrictRequest square{2,DistrictUse::Civic,100,500,true};square.preferredCell=2;
    castle.allowLowerTerraces=true;
    check(generateCityMasterPlan({summit,civic,lower,outer},{castle,square},{2},settings,p,&error) &&
          p.districts.front().anchorCell==3,
          "a summit too small for its castle falls back to a connected defensible shelf");
  }
  reject({a}, {{1, DistrictUse::Harbor, 100, 1000, true}}, {1}, "required harbor cannot use inland land");
  reject({a}, {{1, DistrictUse::Castle, 100, 1000, true}}, {1}, "required castle needs defense");
  reject({a}, {{1, DistrictUse::Prison, 100, 1000, true}}, {1}, "prison requires secure land");
  b.navigableShore = true;
  reject({a, b}, {{1, DistrictUse::Harbor, 100, 1000, true}}, {1}, "corner contact is not an access portal");
  b.boundary = rect(19, 0);
  reject({a, b}, {home}, {1}, "overlapping polygons");
  b.boundary = rect(20, 19);
  reject({a, b}, {{1, DistrictUse::Harbor, 100, 1000, true}}, {1}, "one metre neck cannot provide four metre access");
  auto bad = a;
  bad.boundary[0].x = std::numeric_limits<double>::quiet_NaN();
  reject({bad}, {home}, {1}, "nonfinite geometry");
  bad = a;
  bad.boundary.push_back(bad.boundary[0]);
  reject({bad}, {home}, {1}, "repeated ring vertex");
  bad = a;
  std::reverse(bad.boundary.begin(), bad.boundary.end());
  reject({bad}, {home}, {1}, "wrong winding");
  bad = a;
  bad.slope = .9;
  reject({bad}, {home}, {1}, "steep gate cell");
  reject({a}, {home}, {9}, "unknown gate");
  reject({a}, {home, home}, {1}, "duplicate request");
  reject({a, a}, {home}, {1}, "duplicate cell ID");
  reject({a}, {{1, DistrictUse::Residential, 500, 600, true}}, {1}, "insufficient connected area");
  reject({a}, {{1, DistrictUse::Residential, 100, 200, true}}, {1}, "anchor exceeds target");
  b = a;
  b.id = 2;
  b.boundary = rect(40, 0);
  reject({a, b}, {home}, {1, 2}, "disconnected gate catchments");
  CityMasterPlan first, second;
  check(generateCityMasterPlan({a}, {home}, {1}, settings, first, &error), "fingerprint baseline");
  auto changed = a;
  changed.fertility = .3;
  check(generateCityMasterPlan({changed}, {home}, {1}, settings, second, &error) && first.inputFingerprint != second.inputFingerprint, "upstream suitability edits invalidate fingerprint");
  home.id = 2;
  check(
      generateCityMasterPlan({a}, {{1, DistrictUse::Harbor, 100, 1000, false}, home}, {1}, settings, sentinel, &error) && sentinel.districts.size() == 1 && sentinel.omissions.size() == 1,
      "optional harbor cannot consume required housing");
  // Equal-size cells between opposite gates: public/commercial anchors must
  // serve both arrivals, rather than clustering at whichever gate is nearest.
  std::vector<CityPlanCell> avenue;
  for (uint64_t i = 0; i < 7; ++i)
  {
    CityPlanCell cell;
    cell.id = i + 1;
    cell.boundary = rect(i * 20, 0);
    avenue.push_back(cell);
  }
  for (auto use : {DistrictUse::Merchant, DistrictUse::Civic, DistrictUse::Guild, DistrictUse::Temple})
  {
    CityMasterPlan centered, permuted;
    bool ok = generateCityMasterPlan(avenue, {{1, use, 400, 400, true}}, {1, 7}, settings, centered, &error);
    check(ok && centered.districts.front().anchorCell == 4, std::string(districtUseName(use)) + " balances opposing arrivals");
    check(generateCityMasterPlan(avenue, {{1, use, 400, 400, true}}, {7, 1, 7}, settings, permuted, &error) && encode(centered) == encode(permuted), "duplicate/permuted gates cannot bias central allocation");
    avenue[3].buildable = false;
    // A second row provides a detour; the direct central cell remains excluded.
    for (uint64_t i = 0; i < 7; ++i)
    {
      CityPlanCell cell;
      cell.id = i + 8;
      cell.boundary = rect(i * 20, 20);
      avenue.push_back(cell);
    }
    check(
        generateCityMasterPlan(avenue, {{1, use, 400, 400, true}}, {1, 7}, settings, permuted, &error) && permuted.districts.front().anchorCell == 11,
        "central allocation follows traversable detour around excluded land");
    avenue.resize(7);
    avenue[3].buildable = true;
  }
  auto defensive = a;
  defensive.defense = .9;
  defensive.secure = false;
  reject({defensive}, {{1, DistrictUse::Citadel, 100, 1000, true}}, {1}, "citadel cannot use insecure defensible land");
  defensive.secure = true;
  defensive.allocatable = false;
  reject({defensive}, {{1, DistrictUse::Citadel, 100, 1000, true}}, {1}, "circulation-only cell cannot host a citadel");
  avenue[3].allocatable = false;
  CityMasterPlan circulationPlan;
  bool circulationOK = generateCityMasterPlan(avenue, {{1, DistrictUse::Guild, 400, 400, true}}, {1, 7}, settings, circulationPlan, &error);
  check(circulationOK && circulationPlan.districts.front().anchorCell != 4 && !circulationPlan.gateConnections.empty(), "circulation-only cells retain through access while excluding allocation");
  std::cout << "CITY_PLAN_TESTS assertions=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
