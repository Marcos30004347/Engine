#include "json.hpp"
#include "procedural/world/WorldCityBuildingRegions.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
using namespace procedural::world::rewrite;
using nlohmann::json;
using Poly = std::vector<Position>;
Poly rect(double x, double z, double w, double h)
{
  return {{x, 0, z}, {x + w, 0, z}, {x + w, 0, z + h}, {x, 0, z + h}};
}
Poly band(Position a, Position b, double w)
{
  double len = std::hypot(b.x - a.x, b.z - a.z), x = -(b.z - a.z) * w / len, z = (b.x - a.x) * w / len;
  return {{a.x - x, 0, a.z - z}, {b.x - x, 0, b.z - z}, {b.x + x, 0, b.z + z}, {a.x + x, 0, a.z + z}};
}
json poly(const Poly &p)
{
  json a = json::array();
  for (auto v : p)
    a.push_back({v.x, v.y, v.z});
  return a;
}
json shape(const CityRegionPolygon &s)
{
  json j;
  j["outer"] = poly(s.outer);
  j["holes"] = json::array();
  for (auto &h : s.holes)
    j["holes"].push_back(poly(h));
  return j;
}
json encode(const CityBuildingRegions &r)
{
  json j;
  j["blocks"] = json::array();
  j["regions"] = json::array();
  for (auto &b : r.blocks)
  {
    json courts = json::array();
    for (auto &c : b.courtyards)
      courts.push_back(shape(c));
    j["blocks"].push_back({{"id", b.id}, {"boundary", shape(b.boundary)}, {"courtyards", courts}, {"rejection", b.rejection}});
  }
  for (auto &v : r.regions)
    j["regions"].push_back(
        {{"id", v.id},
         {"block", v.block},
         {"district", v.district},
         {"use", unsigned(v.use)},
         {"footprint", shape(v.footprint)},
         {"passage", poly(v.passage)},
         {"streetAccess", poly({v.streetAccess})},
         {"frontageEdge", v.frontageEdge},
         {"area", v.area}});
  return j;
}
bool overlap(const Poly &a, const Poly &b)
{
  for (auto *p : {&a, &b})
    for (size_t i = 0; i < p->size(); ++i)
    {
      auto u = (*p)[i], v = (*p)[(i + 1) % p->size()];
      auto &o = p == &a ? b : a;
      bool separate = true;
      for (auto q : o)
        if ((v.x - u.x) * (q.z - u.z) - (v.z - u.z) * (q.x - u.x) > 1e-6)
        {
          separate = false;
          break;
        }
      if (separate)
        return false;
    }
  return true;
}
int main(int argc, char **argv)
{
  int assertions = 0, failures = 0;
  auto check = [&](bool ok, const std::string &text)
  {
    ++assertions;
    failures += !ok;
    std::cout << (ok ? "PASS " : "FAIL ") << text << '\n';
  };
  std::filesystem::path output = argc > 1 ? argv[1] : "";
  if (!output.empty())
    std::filesystem::create_directories(output);
  auto terrain = [](double, double)
  {
    return 0.;
  };
  SurfaceMapSettings bounds;
  bounds.minX = -5;
  bounds.minZ = -5;
  bounds.maxX = 165;
  bounds.maxZ = 105;
  CityBuildingRegionSettings settings;
  std::string error;
  {
    std::vector<SurfaceContour> touching;
    for (auto p : std::vector<Poly>{rect(0, 0, 20, 20), rect(20, 20, 20, 20)})
    {
      SurfaceContour c;
      c.id = touching.size() + 1;
      c.material = 11;
      c.loop = p;
      touching.push_back(c);
    }
    SurfaceArrangement map;
    std::vector<CityStreetBlock> blocks;
    bool ok = arrangeSurfaceContours(touching, bounds, terrain, 1, map, &error) && extractCityStreetBlocks(map, 11, blocks, &error);
    check(ok && blocks.size() == 2, "point-touching street blocks stay separate instead of acquiring point access: " + error);
    double total = 0;
    for (const auto &block : blocks)
    {
      const auto &p = block.boundary.outer;
      for (size_t i = 0; i < p.size(); ++i)
        total += (p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x) * .5;
    }
    check(std::abs(total - 800) < 1e-6, "point-contact extraction conserves the full land area");
  }
  SurfaceArrangement baseline;
  for (int scene = 0; scene < 4; ++scene)
  {
    error.clear();
    std::vector<SurfaceContour> contours;
    uint64_t id = 1;
    auto add = [&](Poly p, uint32_t mat, SurfaceRole role = SurfaceRole::Ground)
    {
      SurfaceContour c;
      c.id = id++;
      c.loop = p;
      c.material = mat;
      c.role = role;
      contours.push_back(c);
    };
    add(rect(8, 8, 144, 84), 11);
    if (scene == 0)
    {
      add(rect(77, 0, 6, 100), 1, SurfaceRole::Road);
      add(rect(0, 47, 160, 6), 1, SurfaceRole::Road);
    }
    if (scene == 1)
    {
      add(band({74, 0, 0}, {86, 0, 100}, 3), 1, SurfaceRole::Road);
      add(rect(0, 47, 160, 6), 1, SurfaceRole::Road);
    }
    if (scene == 2)
    {
      for (int k = 0; k < 5; ++k)
        add(band({80 + 7 * std::sin(k * .8), 0, k * 20.}, {80 + 7 * std::sin((k + 1) * .8), 0, (k + 1) * 20.}, 3.5), 1, SurfaceRole::Road);
      add(rect(0, 47, 160, 6), 1, SurfaceRole::Road);
    }
    if (scene == 3)
    {
      add(rect(77, 0, 6, 100), 1, SurfaceRole::Road);
      add(rect(0, 47, 160, 6), 1, SurfaceRole::Road);
      add(rect(130, 0, 30, 100), 9, SurfaceRole::Water);
      add(rect(22, 65, 8, 8), 9, SurfaceRole::Water);
    }
    SurfaceArrangement map;
    check(arrangeSurfaceContours(contours, bounds, terrain, 7, map, &error), "scene " + std::to_string(scene) + " source map " + error);
    if (scene == 0)
      baseline = map;
    CityBuildingRegions regions;
    bool ok = generateCityBuildingRegions(map, settings, terrain, regions, &error);
    check(ok, "scene " + std::to_string(scene) + " generate regions " + error);
    if (!ok)
      continue;
    check(regions.blocks.size() == 4, "four blocks derived from final road complement");
    if (scene == 3)
    {
      bool holes = false;
      for (auto &b : regions.blocks)
        holes |= !b.boundary.holes.empty();
      check(holes, "internal water hole retained in street block");
    }
    check(regions.regions.size() >= 3, "connected perimeter massing replaces isolated houses");
    CityBuildingRegions repeat;
    auto permuted = map;
    std::reverse(permuted.edges.begin(), permuted.edges.end());
    check(generateCityBuildingRegions(permuted, settings, terrain, repeat, &error) && encode(repeat) == encode(regions), "edge-order-independent region masks and access");
    bool metadata = true, passages = true, clear = true;
    double roofArea = 0, regionArea = 0;
    for (auto &r : regions.regions)
    {
      metadata &= r.district == settings.district && r.frontageEdge != 0 && r.area >= settings.minimumArea;
      passages &= r.passage.size() == 4;
      regionArea += r.area;
    }
    for (auto &f : regions.foundations)
      for (auto &c : contours)
        if (c.role == SurfaceRole::Road || c.role == SurfaceRole::Water)
          clear &= !overlap(f.loop, c.loop);
    for (auto &f : regions.foundations)
      for (auto &r : regions.regions)
        clear &= !overlap(f.loop, r.passage);
    for (auto &t : regions.triangles)
      if (t.material == 13)
      {
        auto a = t.points[0], b = t.points[1], c = t.points[2];
        double ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
        roofArea += ny * .5;
      }
    bool setback = true;
    for (const auto &r : regions.regions)
    {
      const auto &block = *std::find_if(
          regions.blocks.begin(),
          regions.blocks.end(),
          [&](auto &b)
          {
            return b.id == r.block;
          });
      auto boundaries = block.boundary.holes;
      boundaries.push_back(block.boundary.outer);
      auto loops = r.footprint.holes;
      loops.push_back(r.footprint.outer);
      for (auto &loop : loops)
        for (auto p : loop)
          for (auto &boundary : boundaries)
            for (size_t k = 0; k < boundary.size(); ++k)
            {
              auto a = boundary[k], b = boundary[(k + 1) % boundary.size()];
              double dx = b.x - a.x, dz = b.z - a.z, t = std::clamp(((p.x - a.x) * dx + (p.z - a.z) * dz) / (dx * dx + dz * dz), 0., 1.);
              setback &= std::hypot(p.x - a.x - dx * t, p.z - a.z - dz * t) >= settings.setback - .003;
            }
    }
    check(setback, "building boundary preserves configured setback from all block edges");
    if (scene == 3)
    {
      bool hole = false;
      for (auto &r : regions.regions)
        hole |= !r.footprint.holes.empty();
      check(hole, "protected hole survives building mask and roof triangulation");
    }
    check(metadata && passages, "WFC metadata and mandatory passage polygons");
    check(clear, "no road water or passage occupied by foundations");
    check(std::abs(roofArea - regionArea) < 1e-5, "roof CDT retains exact building mask area");
    auto combined = contours;
    for (auto f : regions.foundations)
    {
      f.id = id++;
      combined.push_back(f);
    }
    SurfaceArrangement finalMap;
    SurfaceArrangementMesh ground;
    check(arrangeSurfaceContours(combined, bounds, terrain, 7, finalMap, &error) && meshSurfaceArrangement(finalMap, terrain, {}, ground, &error), "building foundation edges preserved by ground CDT " + error);
    check(std::abs(ground.groundArea - 18700) < 1e-5, "full ground coverage");
    if (!output.empty())
    {
      auto j = encode(regions);
      j["sourceContours"] = json::array();
      for (auto &c : contours)
        j["sourceContours"].push_back({{"material", c.material}, {"loop", poly(c.loop)}});
      std::ofstream(output / (std::to_string(scene) + ".json")) << j.dump(2);
      json packed;
      packed["phase"] = 11;
      auto &m = packed["mesh"];
      m["vertices"] = json::array();
      m["triangles"] = json::array();
      m["materials"] = json::array();
      m["expectedNormals"] = json::array();
      double omittedArea = 0;
      auto emit = [&](Position a, Position b, Position c, uint32_t mat)
      {
        double nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y), ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z), nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x),
               l = std::sqrt(nx * nx + ny * ny + nz * nz);
        float ux = float(b.x) - float(a.x), uy = float(b.y) - float(a.y), uz = float(b.z) - float(a.z), vx = float(c.x) - float(a.x), vy = float(c.y) - float(a.y), vz = float(c.z) - float(a.z), fx = uy * vz - uz * vy,
              fy = uz * vx - ux * vz, fz = ux * vy - uy * vx;
        double fl = std::sqrt(double(fx) * fx + double(fy) * fy + double(fz) * fz);
        if (l == 0 || fl <= 1e-8 || fx * nx + fy * ny + fz * nz <= 0)
        {
          omittedArea += l * .5;
          return;
        }
        int i = m["vertices"].size();
        for (auto p : {a, b, c})
          m["vertices"].push_back({p.x, p.y, p.z});
        m["triangles"].push_back({i, i + 1, i + 2});
        m["materials"].push_back(mat);
        m["expectedNormals"].push_back({nx / l, ny / l, nz / l});
      };
      for (auto &t : ground.triangles)
        emit(ground.vertices[t.vertices[0]], ground.vertices[t.vertices[1]], ground.vertices[t.vertices[2]], t.material == 12 ? 11 : t.material);
      for (auto &t : regions.triangles)
        emit(t.points[0], t.points[1], t.points[2], t.material);
      check(omittedArea < .0001, "float export retains area within one square centimetre");
      packed["renderOmittedAreaMetres2"] = omittedArea;
      std::ofstream(output / (std::to_string(scene) + "-mesh.json")) << packed.dump();
    }
    std::cout << "BUILDING_REGION_METRIC scene=" << scene << " blocks=" << regions.blocks.size() << " regions=" << regions.regions.size() << " area=" << regionArea << " massing_triangles=" << regions.triangles.size()
              << '\n';
  }
  CityBuildingRegions sentinel;
  sentinel.revision = 999;
  auto bad = settings;
  bad.passageWidth = 0;
  check(!generateCityBuildingRegions(baseline, bad, terrain, sentinel, &error) && sentinel.revision == 999, "zero-width access fails atomically");
  bad = settings;
  bad.depth = std::numeric_limits<double>::quiet_NaN();
  check(!generateCityBuildingRegions(baseline, bad, terrain, sentinel, &error) && sentinel.revision == 999, "nonfinite depth fails atomically");
  auto invalid = baseline;
  invalid.edges[0].a = 999999;
  check(!generateCityBuildingRegions(invalid, settings, terrain, sentinel, &error) && sentinel.revision == 999, "invalid map edge fails atomically");
  bad = settings;
  bad.depth = 35;
  CityBuildingRegions rejected;
  check(generateCityBuildingRegions(baseline, bad, terrain, rejected, &error) && rejected.regions.empty() && !rejected.blocks[0].rejection.empty(), "small blocks explicitly reject oversized courtyards");
  auto noRoad = baseline;
  for (auto &f : noRoad.faces)
    if (f.role == SurfaceRole::Road)
      f.role = SurfaceRole::Ground;
  check(generateCityBuildingRegions(noRoad, settings, terrain, rejected, &error) && rejected.regions.empty(), "unserved blocks do not invent street frontage");
  check(
      generateCityBuildingRegions(
          baseline,
          settings,
          [](double x, double)
          {
            return x * .2;
          },
          rejected,
          &error) &&
          rejected.regions.empty(),
      "steep foundations require engineering");
  invalid = baseline;
  invalid.vertices[0].x = std::numeric_limits<double>::quiet_NaN();
  check(!generateCityBuildingRegions(invalid, settings, terrain, sentinel, &error) && sentinel.revision == 999, "nonfinite map geometry fails atomically");
  invalid = baseline;
  for (auto &f : invalid.faces)
    if (f.material == settings.landMaterial)
    {
      f.role = SurfaceRole::Water;
      break;
    }
  check(!generateCityBuildingRegions(invalid, settings, terrain, sentinel, &error) && sentinel.revision == 999, "protected role cannot be configured as buildable land");
  std::cout << "BUILDING_REGION_TESTS assertions=" << assertions << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
