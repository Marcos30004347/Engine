#include "json.hpp"
#include "procedural/world/WorldCityWalls.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
using namespace procedural::world::rewrite;
using nlohmann::json;
std::vector<Position> rect(double x, double z, double w, double h)
{
  return {{x, 0, z}, {x + w, 0, z}, {x + w, 0, z + h}, {x, 0, z + h}};
}
json poly(const std::vector<Position> &p)
{
  json a = json::array();
  for (auto v : p)
    a.push_back({v.x, v.y, v.z});
  return a;
}
json encode(const CityWalls &w)
{
  json j;
  j["circuits"] = json::array();
  j["gates"] = json::array();
  j["foundations"] = json::array();
  for (auto &c : w.circuits)
    j["circuits"].push_back({{"era", c.era}, {"boundary", poly(c.boundary)}, {"perimeter", c.perimeter}, {"openings", c.openingLength}});
  for (auto &g : w.gates)
    j["gates"].push_back({{"id", g.id}, {"era", g.era}, {"routes", g.routes}, {"ends", poly({g.a, g.b})}});
  for (auto &f : w.foundations)
    j["foundations"].push_back(poly(f.loop));
  return j;
}
int main(int argc, char **argv)
{
  int assertions = 0, failures = 0;
  auto check = [&](bool ok, std::string name)
  {
    ++assertions;
    failures += !ok;
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
  };
  std::filesystem::path output = argc > 1 ? argv[1] : "";
  if (!output.empty())
    std::filesystem::create_directories(output);
  auto flat = [](double, double)
  {
    return 0.;
  };
  CityWallSettings settings;
  CityMasterPlan plan;
  plan.revision = 4;
  std::vector<CityPlanCell> cells;
  for (int z = 0; z < 7; ++z)
    for (int x = 0; x < 7; ++x)
    {
      CityPlanCell c;
      c.id = z * 7 + x + 1;
      c.boundary = rect(x * 20, z * 20, 20, 20);
      cells.push_back(c);
    }
  // District core consumed by ID; infill expands each era without convex hulls.
  CityPlanDistrict castle;
  castle.id = 1;
  castle.cells = {25};
  castle.pieces = {cells[24].boundary};
  plan.districts = {castle};
  std::vector<CityWallEra> eras{{1, {1}, {}}, {2, {}, {}}, {3, {}, {}}};
  for (int z = 2; z <= 4; ++z)
    for (int x = 2; x <= 4; ++x)
      eras[1].infillCells.push_back(z * 7 + x + 1);
  for (int z = 1; z <= 5; ++z)
    for (int x = 1; x <= 5; ++x)
      eras[2].infillCells.push_back(z * 7 + x + 1);
  std::vector<CityWallRoute> roads{{1, 2, {{5, 0, 70}, {135, 0, 70}}}, {2, 2, {{70, 0, 5}, {70, 0, 135}}}};
  CityWalls baseline;
  std::string error;
  for (int scenario = 0; scenario < 4; ++scenario)
  {
    auto e = eras;
    auto r = roads;
    auto fixtureCells = cells;
    if (scenario == 3)
      for (auto &cell : fixtureCells)
        for (auto &p : cell.boundary)
        {
          double x = p.x, z = p.z;
          p.x = x + 2 * std::sin(z * .07) + 1.5 * std::sin(x * .09);
          p.z = z + 2 * std::sin(x * .05);
        }
    if (scenario == 1)
    { // An irregular outer enclosure with a stepped extension.
      e[2].infillCells.push_back(3 * 7 + 6 + 1);
      e[2].infillCells.push_back(2 * 7 + 6 + 1);
      r[0].path.back().x = 155;
    }
    if (scenario == 2)
    {
      r = {{1, 2, {{5, 0, 66}, {135, 0, 74}}}, {2, 2, {{70, 0, 5}, {70, 0, 135}}}};
    }
    CityWalls w;
    bool ok = generateCityWalls(plan, fixtureCells, e, r, settings, flat, w, &error);
    check(ok, "scenario " + std::to_string(scenario) + " walls " + error);
    if (!ok)
      continue;
    if (scenario == 0)
      baseline = w;
    check(w.circuits.size() == 3 && w.gates.size() == 12, "three circuits and four gates each");
    check(!w.triangles.empty() && !w.foundations.empty(), "structural wall triangles and map foundations");
    check((scenario != 1 && scenario != 3) || w.circuits.back().boundary.size() > 4, "union retains concave historical footprint");
    bool dimensions = true;
    for (auto &g : w.gates)
      dimensions &= std::hypot(g.a.x - g.b.x, g.a.z - g.b.z) >= 6 - 1e-6;
    check(dimensions, "gate opening includes road width and clearance");
    auto reverseCells = fixtureCells;
    std::reverse(reverseCells.begin(), reverseCells.end());
    std::reverse(e.begin(), e.end());
    std::reverse(r.begin(), r.end());
    CityWalls repeated;
    check(generateCityWalls(plan, reverseCells, e, r, settings, flat, repeated, &error) && encode(w) == encode(repeated), "input permutation preserves circuits gates and foundations");
    SurfaceMapSettings ms;
    ms.minX = -10;
    ms.minZ = -10;
    ms.maxX = 160;
    ms.maxZ = 150;
    SurfaceArrangement map;
    SurfaceArrangementMesh ground;
    check(arrangeSurfaceContours(w.foundations, ms, flat, 4, map, &error) && meshSurfaceArrangement(map, flat, {}, ground, &error), "foundation contours survive arrangement and CDT " + error);
    check(std::abs(ground.groundArea - 27200) < 1e-5, "ground coverage preserved around wall foundations");
    // At each wall centerline, every crossing route occupies a gate interval.
    bool open = true;
    for (auto &c : w.circuits)
      for (auto &route : r)
      {
        unsigned n = 0;
        for (auto &g : w.gates)
          if (g.era == c.era && std::find(g.routes.begin(), g.routes.end(), route.id) != g.routes.end())
            ++n;
        open &= n == 2;
      }
    check(open, "both road axes pass through every successive enclosure");
    if (!output.empty())
    {
      auto j = encode(w);
      j["routes"] = json::array();
      for (auto &road : r)
        j["routes"].push_back(poly(road.path));
      std::ofstream(output / (std::to_string(scenario) + ".json")) << j.dump(2);
      json packed;
      packed["phase"] = 11;
      auto &mesh = packed["mesh"];
      mesh["vertices"] = json::array();
      mesh["triangles"] = json::array();
      mesh["materials"] = json::array();
      mesh["expectedNormals"] = json::array();
      double omittedArea = 0;
      unsigned omittedTriangles = 0;
      auto emit = [&](Position a, Position b, Position c, uint32_t mat)
      {
        double nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y), ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z), nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x),
               l = std::sqrt(nx * nx + ny * ny + nz * nz);
        float ux = float(b.x) - float(a.x), uy = float(b.y) - float(a.y), uz = float(b.z) - float(a.z);
        float vx = float(c.x) - float(a.x), vy = float(c.y) - float(a.y), vz = float(c.z) - float(a.z);
        float fx = uy * vz - uz * vy, fy = uz * vx - ux * vz, fz = ux * vy - uy * vx;
        double packedLength = std::sqrt(double(fx) * fx + double(fy) * fy + double(fz) * fz);
        if (l == 0 || packedLength <= 1e-8 || fx * nx + fy * ny + fz * nz <= 0)
        {
          omittedArea += l * .5;
          ++omittedTriangles;
          return;
        }
        int i = mesh["vertices"].size();
        for (auto p : {a, b, c})
          mesh["vertices"].push_back({p.x, p.y, p.z});
        mesh["triangles"].push_back({i, i + 1, i + 2});
        mesh["materials"].push_back(mat);
        mesh["expectedNormals"].push_back({nx / l, ny / l, nz / l});
      };
      for (auto &t : ground.triangles)
        emit(ground.vertices[t.vertices[0]], ground.vertices[t.vertices[1]], ground.vertices[t.vertices[2]], 0);
      for (auto &t : w.triangles)
        emit(t.points[0], t.points[1], t.points[2], 12);
      check(omittedArea < .0001, "native float conversion omits less than one square centimetre");
      packed["renderOmittedAreaMetres2"] = omittedArea;
      packed["renderOmittedTinyTriangles"] = omittedTriangles;
      std::ofstream(output / (std::to_string(scenario) + "-mesh.json")) << packed.dump();
    }
    std::cout << "WALL_METRIC scenario=" << scenario << " circuits=" << w.circuits.size() << " gates=" << w.gates.size() << " structural_triangles=" << w.triangles.size() << '\n';
  }
  auto reject = [&](auto c, auto e, auto r, const std::string &name)
  {
    CityWalls w;
    w.revision = 999;
    check(!generateCityWalls(plan, c, e, r, settings, flat, w, &error) && w.revision == 999 && !error.empty(), name + " fails atomically");
  };
  auto e = eras;
  e[1].id = 1;
  reject(cells, e, roads, "duplicate era");
  e = eras;
  e[0].districts = {99};
  reject(cells, e, roads, "unknown district");
  e = eras;
  e[0].infillCells = {999};
  reject(cells, e, roads, "unknown cell");
  e = eras;
  e[0].infillCells = {1};
  reject(cells, e, roads, "disconnected union");
  e = eras;
  e[1].infillCells.clear();
  reject(cells, e, roads, "coincident historical walls");
  auto r = roads;
  r[0].path = {{5, 0, 60}, {135, 0, 60}};
  reject(cells, eras, r, "corner gate");
  r = roads;
  r[0].path = {{5, 0, 60.5}, {135, 0, 60.5}};
  reject(cells, eras, r, "insufficient corner pier");
  r = {{1, 2, {{5, 0, 5}, {10, 0, 5}}}};
  reject(cells, eras, r, "inaccessible enclosure");
  r = roads;
  r[0].path[0].x = std::numeric_limits<double>::quiet_NaN();
  reject(cells, eras, r, "nonfinite route");
  auto c = cells;
  c[24].buildable = false;
  reject(c, eras, roads, "excluded core land");
  CityWalls w;
  w.revision = 999;
  check(
      !generateCityWalls(
          plan,
          cells,
          eras,
          roads,
          settings,
          [](double x, double)
          {
            return std::sin(x);
          },
          w,
          &error) &&
          w.revision == 999,
      "nonplanar foundation requires engineering");
  r = {{1, 2, {{5, 0, 70}, {60, 0, 70}}}};
  reject(cells, eras, r, "road terminates at inner wall without crossing");
  r = roads;
  r.push_back({3, 1, {{65, 0, 57.5}, {75, 0, 57.5}}});
  reject(cells, eras, r, "noncrossing road width clips wall");
  e = eras;
  e[0].districts.clear();
  e[0].infillCells = {17, 18, 19, 24, 26, 31, 32, 33};
  reject(cells, e, roads, "courtyard hole needs explicit independent defense policy");
  e = eras;
  e[0].infillCells = {17};
  reject(cells, e, roads, "point-touching defended components");
  auto localSettings = settings;
  localSettings.height = std::numeric_limits<double>::quiet_NaN();
  check(!generateCityWalls(plan, cells, eras, roads, localSettings, flat, w, &error), "nonfinite wall dimensions rejected");
  check(
      generateCityWalls(
          plan,
          cells,
          eras,
          roads,
          settings,
          [](double x, double z)
          {
            return .01 * x + .005 * z;
          },
          w,
          &error),
      "engineered affine slope seats wall foundations");
  // Two close routes merge gate openings instead of leaving a needle pier.
  r = {{1, 1, {{5, 0, 68}, {135, 0, 68}}}, {2, 1, {{5, 0, 71}, {135, 0, 71}}}};
  check(generateCityWalls(plan, cells, eras, r, settings, flat, w, &error) && w.gates.size() == 6 && w.gates[0].routes.size() == 2, "nearby crossings merge into shared gates " + error);
  std::cout << "WALL_TESTS assertions=" << assertions << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
