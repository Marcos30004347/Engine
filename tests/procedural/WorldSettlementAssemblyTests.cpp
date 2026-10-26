#include "json.hpp"
#include "procedural/world/WorldSettlementAssembly.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
using namespace procedural::world::rewrite;
using nlohmann::json;
int main(int argc, char **argv)
{
  int failures = 0, checks = 0;
  auto check = [&](bool b, std::string s)
  {
    ++checks;
    failures += !b;
    std::cout << (b ? "PASS " : "FAIL ") << s << '\n';
  };
  SettlementAssemblyInput in;
  in.earthworks.map.minX = -30;
  in.earthworks.map.minZ = -30;
  in.earthworks.map.maxX = 270;
  in.earthworks.map.maxZ = 270;
  in.roads.halfWidthMetres = 2;
  in.roads.shoulderMetres = .5;
  in.roads.crownSlope = 0;
  in.refinement.groundSpacingMetres = 20;
  in.refinement.modifiedSpacingMetres = 8;
  for (int z = 0; z < 3; ++z)
    for (int x = 0; x < 3; ++x)
    {
      CityPlanCell c;
      c.id = z * 3 + x + 1;
      double a = x * 80, b = z * 80;
      c.boundary = {{a, 0, b}, {a + 80, 0, b}, {a + 80, 0, b + 80}, {a, 0, b + 80}};
      c.defense = (x == 1 && z == 1) ? 1 : 0;
      c.secure = true;
      in.cells.push_back(c);
    }
  in.districts = {{101, DistrictUse::Castle, 6400, 6400, true}, {202, DistrictUse::Merchant, 6400, 12800, true}, {303, DistrictUse::Residential, 6400, 38400, true}};
  in.gateCells = {1, 9};
  in.eras = {{1, {101}, {}}, {2, {}, {1, 2, 3, 4, 5, 6, 7, 8, 9}}};
  for (int axis = 0; axis < 2; ++axis)
    for (int k = 0; k < 3; ++k)
    {
      std::vector<Position> p;
      for (int j = 0; j <= 7; ++j)
      {
        double t = -20 + j * 40;
        p.push_back(axis ? Position{40. + 80 * k, 0, t} : Position{t, 0, 40. + 80 * k});
      }
      in.streets.push_back(p);
    }
  auto flat = [](double, double)
  {
    return 0.;
  };
  std::string error;
  SettlementAssembly result;
  auto exportMesh = [&](const SettlementAssembly &result, const std::string &stem)
  {
    if (argc <= 1)
      return;
    std::filesystem::create_directories(argv[1]);
    json mesh;
    mesh["vertices"] = json::array();
    mesh["triangles"] = json::array();
    mesh["materials"] = json::array();
    mesh["expectedNormals"] = json::array();
    double omitted = 0;
    auto tri = [&](std::array<Position, 3> p, uint32_t mat)
    {
      auto a = p[0], b = p[1], c = p[2];
      double x = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y), y = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z), z = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x),
             l = std::sqrt(x * x + y * y + z * z);
      float ux = float(b.x) - float(a.x), uy = float(b.y) - float(a.y), uz = float(b.z) - float(a.z);
      float vx = float(c.x) - float(a.x), vy = float(c.y) - float(a.y), vz = float(c.z) - float(a.z);
      float fx = uy * vz - uz * vy, fy = uz * vx - ux * vz, fz = ux * vy - uy * vx;
      if (l == 0 || std::sqrt(fx * fx + fy * fy + fz * fz) <= 1e-8f || fx * x + fy * y + fz * z <= 0)
      {
        omitted += l / 2;
        return;
      }
      auto n = mesh["vertices"].size();
      for (auto v : p)
        mesh["vertices"].push_back({v.x, v.y, v.z});
      mesh["triangles"].push_back({n, n + 1, n + 2});
      mesh["materials"].push_back(mat);
      mesh["expectedNormals"].push_back({x / l, y / l, z / l});
    };
    for (auto t : result.mesh.triangles)
      tri({result.mesh.vertices[t.vertices[0]], result.mesh.vertices[t.vertices[1]], result.mesh.vertices[t.vertices[2]]}, t.material >= 20 || t.material == 12 ? 11 : t.material);
    for (auto t : result.walls.triangles)
      tri(t.points, 12);
    for (auto t : result.buildings.triangles)
      tri(t.points, t.material);
    std::ofstream(std::filesystem::path(argv[1]) / (stem + "-mesh.json")) << json{{"phase", 11}, {"mesh", mesh}, {"renderOmittedAreaMetres2", omitted}}.dump();
    check(omitted < .0001, "render omission area below tolerance");
  };
  bool ok = assembleSettlement(in, flat, result, &error);
  check(ok, "integrated settlement: " + error);
  if (ok)
  {
    check(result.plan.districts.size() == 3, "three semantic districts realized");
    check(result.walls.circuits.size() == 2 && result.walls.gates.size() == 16, "two wall eras with sixteen engineered road gates");
    check(std::abs(result.mesh.groundArea - 90000) < .01, "final CDT conserves map area");
    std::set<uint64_t> ids, blocks, districts;
    for (auto &b : result.buildings.blocks)
      blocks.insert(b.id);
    bool references = true;
    for (auto &b : result.buildings.regions)
    {
      references &= ids.insert(b.id).second && blocks.count(b.block) && b.frontageEdge > 0 && b.frontageEdge <= result.map.edges.size();
      districts.insert(b.district);
    }
    check(references && districts.size() == 3, "unique region IDs, district ownership and final frontage references");
    bool shoulders = false;
    for (auto &b : result.buildings.regions)
    {
      auto &e = result.map.edges[b.frontageEdge - 1];
      shoulders |= result.map.faces[e.leftFace - 1].role == SurfaceRole::Shoulder || result.map.faces[e.rightFace - 1].role == SurfaceRole::Shoulder;
    }
    check(shoulders, "courtyard passages connect to engineered shoulders");
    std::set<uint64_t> contourIds;
    bool uniqueContours = true;
    for (auto &c : result.walls.foundations)
      uniqueContours &= contourIds.insert(c.id).second;
    for (auto &c : result.buildings.foundations)
      uniqueContours &= contourIds.insert(c.id).second;
    check(uniqueContours, "published foundation IDs share final map namespace");
    auto overlap = [](const std::vector<Position> &a, const std::vector<Position> &b)
    {
      for (auto *p : {&a, &b})
        for (size_t i = 0; i < p->size(); ++i)
        {
          auto u = (*p)[i], v = (*p)[(i + 1) % p->size()];
          auto &other = p == &a ? b : a;
          bool separated = true;
          double winding = 0;
          for (size_t j = 0; j < p->size(); ++j)
          {
            auto a = (*p)[j], b = (*p)[(j + 1) % p->size()];
            winding += a.x * b.z - a.z * b.x;
          }
          for (auto q : other)
            if (((v.x - u.x) * (q.z - u.z) - (v.z - u.z) * (q.x - u.x)) * (winding > 0 ? 1 : -1) > 1e-6)
            {
              separated = false;
              break;
            }
          if (separated)
            return false;
        }
      return true;
    };
    bool clearance = true;
    for (auto &b : result.buildings.foundations)
    {
      for (auto &w : result.walls.foundations)
        clearance &= !overlap(b.loop, w.loop);
      for (auto &road : result.roads.roads)
        for (auto &t : road.design.triangles)
          clearance &= !overlap(b.loop, {t.points.begin(), t.points.end()});
    }
    check(clearance, "building foundations avoid all wall solids and road shoulders");
    SettlementAssembly repeated;
    std::string repeatError;
    auto reordered = in;
    std::reverse(reordered.cells.begin(), reordered.cells.end());
    std::reverse(reordered.districts.begin(), reordered.districts.end());
    bool repeat = assembleSettlement(reordered, flat, repeated, &repeatError);
    bool equal = repeat && repeated.mesh.vertices.size() == result.mesh.vertices.size() && repeated.mesh.triangles.size() == result.mesh.triangles.size();
    if (equal)
      for (size_t k = 0; k < result.mesh.vertices.size(); ++k)
      {
        auto a = result.mesh.vertices[k], b = repeated.mesh.vertices[k];
        equal &= a.x == b.x && a.y == b.y && a.z == b.z;
      }
    check(equal, "permuted cell and district input produces identical CDT vertices and counts");
    exportMesh(result, "settlement");
  }
  auto skewed = in;
  for (auto &c : skewed.cells)
    for (auto &p : c.boundary)
      p.x += .12 * p.z;
  for (auto &road : skewed.streets)
    for (auto &p : road)
      p.x += .12 * p.z;
  for (auto &road : skewed.streets)
  {
    std::vector<Position> fine;
    for (size_t k = 1; k < road.size(); ++k)
    {
      auto a = road[k - 1], b = road[k];
      fine.push_back(a);
      fine.push_back({(a.x + b.x) / 2, 0, (a.z + b.z) / 2});
    }
    fine.push_back(road.back());
    road = std::move(fine);
  }
  for (auto &road : skewed.streets)
    for (auto &p : road)
      p.y = 6;
  skewed.earthworks.map.minX = -40;
  skewed.earthworks.map.maxX = 310;
  SettlementAssembly oblique;
  error.clear();
  bool skewOK = assembleSettlement(
      skewed,
      [](double, double)
      {
        return 6.;
      },
      oblique,
      &error);
  check(skewOK, "oblique streets and gates on elevated platform: " + error);
  if (skewOK)
    check(oblique.walls.gates.size() == 16 && std::abs(oblique.mesh.groundArea - 105000) < .01, "oblique settlement preserves gates and domain area");
  if (skewOK)
    exportMesh(oblique, "oblique-streets");
  auto bad = in;
  bad.streets.resize(1);
  auto previous = result.mesh.triangles.size();
  error.clear();
  check(!assembleSettlement(bad, flat, result, &error) && result.mesh.triangles.size() == previous, "unserved district rejects atomically: " + error);
  bad = in;
  bad.streets.pop_back();
  bad.streets.pop_back();
  bad.streets.pop_back();
  error.clear();
  check(!assembleSettlement(bad, flat, result, &error), "disconnected roads reject");
  bad = in;
  bad.buildings.depth = 100;
  error.clear();
  check(!assembleSettlement(bad, flat, result, &error), "unrealizable district rejects");
  std::cout << "assertions=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
