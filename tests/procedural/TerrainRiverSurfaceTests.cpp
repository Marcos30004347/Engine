#include "procedural/Geometry.hpp"
#include "procedural/geometry/TerrainGuides.hpp"
#include "procedural/geometry/TerrainRiverSurface.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
using namespace procedural;
using namespace procedural::geometry;
int main(int argc, char **argv)
{
  int failures = 0;
  const auto check = [&](bool value, const char *label)
  {
    std::cout << (value ? "PASS " : "FAIL ") << label << '\n';
    failures += !value;
  };
  TerrainRiverSurface river;
  const std::vector<RiverSurfaceReach> reaches = {
    {{300, 700}, {1500, 1800}, 16, 50, 35, 1, 2}, {{400, 2900}, {1500, 1800}, 48, 45, 35, 2, 2}, {{1500, 1800}, {2500, 2000}, 64, 35, 20, 2, 3}, {{2500, 2000}, {3700, 2400}, 100, 20, 10, 3, 3, false, true}};
  river.build(reaches, 4000, 17);
  const auto &s = river.sections();
  check(s.size() == 4 && s.back().widthB > s.front().widthA * 2, "discharge expands river width");
  check(distance(s[2].leftB, s[3].leftA) < 1e-8 && distance(s[2].rightB, s[3].rightA) < 1e-8, "both variable-width banks join at bends");
  bool downhill = true, wet = true, banks = true;
  for (const auto &r : s)
  {
    downhill &= r.levelA >= r.levelB;
    for (int i = 1; i < 9; ++i)
    {
      const double t = i / 10.;
      const Point2 p{r.a.x + (r.b.x - r.a.x) * t, r.a.y + (r.b.y - r.a.y) * t};
      const auto v = river.sample(p);
      wet &= v.inside && v.depth > 0 && std::abs(v.lateral) < 1e-6;
    }
    banks &= river.sample(r.leftA).inside && river.sample(r.leftB).inside && river.sample(r.rightA).inside && river.sample(r.rightB).inside;
  }
  check(downhill && s.back().levelB == 10, "water descends to the fixed outlet despite changing depth");
  check(wet && banks, "carving and water use the same complete bank ribbon");
  check(river.chains().size() == 20, "both banks, centre and cross sections become CDT constraints");
  std::vector<CorridorGuide> corridors;
  for (const auto &r : s)
    corridors.push_back({r.a, r.b, r.widthA, 100, r.widthB, 100});
  const auto bankGuides = riverSurfaceGuides(corridors, 10000);
  bool sameBanks = true;
  for (size_t i = 0; i < s.size(); ++i)
    sameBanks &= distance(bankGuides[i * 9 + 4].points.front(), s[i].leftA) < 1e-8 && distance(bankGuides[i * 9 + 4].points.back(), s[i].leftB) < 1e-8;
  check(sameBanks, "terrain shoulder guides share the exact variable-width water banks");
  TerrainMeshSettings settings;
  settings.tileSizeMetres = 2000;
  settings.borderSpacingMetres = 200;
  settings.maximumConstraintSegmentMetres = 100;
  settings.workerCount = 2;
  TerrainMesh mesh;
  std::string error;
  check(river.buildMesh(settings, mesh, &error), "separate river mesh triangulates");
  if (!error.empty())
    std::cout << error << '\n';
  bool contained = true, heights = true;
  size_t triangles = 0;
  for (const auto &tile : mesh.tiles)
  {
    for (const auto &v : tile.vertices)
    {
      auto p = river.sample({v.x, v.z});
      heights &= p.inside && std::abs(p.level - v.y) < 1e-7;
    }
    for (size_t i = 0; i < tile.indices.size(); i += 3)
    {
      const auto a = tile.vertices[tile.indices[i]], b = tile.vertices[tile.indices[i + 1]], c = tile.vertices[tile.indices[i + 2]];
      contained &= river.sample({(a.x + b.x + c.x) / 3, (a.z + b.z + c.z) / 3}).inside;
      ++triangles;
    }
  }
  std::cout << "faces " << triangles << " contained " << contained << " heights " << heights << "\n";
  check(triangles > 20 && contained && heights, "water mesh contains only river faces at the shared water levels");
  TerrainMesh again;
  check(river.buildMesh(settings, again, &error) && again.statistics.triangleCount == triangles, "first and repeated builds agree");
  TerrainMesh streamed;
  size_t emitted = 0;
  settings.retainTiles = false;
  settings.tileSink = [&](MeshTile &t, std::string &)
  {
    emitted += t.triangleCount();
    return true;
  };
  settings.workerCount = 1;
  check(river.buildMesh(settings, streamed, &error) && emitted == triangles && streamed.statistics.triangleCount == triangles, "streamed river triangles match retained output");
  std::cout << "streamed " << emitted << " stats " << streamed.statistics.triangleCount << " error " << error << "\n";
  TerrainMesh clipped;
  settings.retainTiles = true;
  settings.tileSink = {};
  Chain coast;
  coast.kind = ChainKind::Coast;
  coast.points = {{3400, 0}, {3400, 4000}};
  check(
      river.buildMesh(
          settings,
          clipped,
          &error,
          {coast},
          [](Point2 p)
          {
            return p.x < 3400;
          }),
      "river mesh clips against lake shoreline constraints");
  bool clippedAtCoast = true;
  for (const auto &tile : clipped.tiles)
    for (const auto &v : tile.vertices)
      clippedAtCoast &= v.x <= 3400.00001;
  check(clippedAtCoast && clipped.statistics.triangleCount < triangles, "river water stops at lake boundary without overlapping its surface");
  if (argc > 1)
  {
    std::ofstream out(argv[1]);
    out << "{\"vertices\":[";
    bool first = true;
    for (const auto &tile : mesh.tiles)
      for (const auto &v : tile.vertices)
      {
        if (!first)
          out << ',';
        first = false;
        out << '[' << v.x << ',' << v.y << ',' << v.z << ']';
      }
    out << "],\"triangles\":[";
    first = true;
    uint32_t offset = 0;
    for (const auto &tile : mesh.tiles)
    {
      for (size_t i = 0; i < tile.indices.size(); i += 3)
      {
        if (!first)
          out << ',';
        first = false;
        out << '[' << offset + tile.indices[i] << ',' << offset + tile.indices[i + 1] << ',' << offset + tile.indices[i + 2] << ']';
      }
      offset += tile.vertices.size();
    }
    out << "]}";
  }
  return failures ? 1 : 0;
}
