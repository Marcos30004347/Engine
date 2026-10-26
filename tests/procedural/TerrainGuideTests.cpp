#include "procedural/Geometry.hpp"
#include "procedural/geometry/TerrainGuides.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <set>
using namespace procedural;
using namespace procedural::geometry;
namespace
{
std::vector<Chain> border()
{
  Chain c;
  c.closed = true;
  c.kind = ChainKind::WorldBorder;
  c.points = {{0, 0}, {8000, 0}, {8000, 8000}, {0, 8000}};
  return {c};
}
void dump(const std::string &path, const TerrainMesh &mesh, const std::vector<Chain> &guides, const HeightFunction &height)
{
  std::ofstream out(path);
  out.precision(15);
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
  out << "],\"guides\":[";
  first = true;
  for (const auto &c : guides)
  {
    if (!first)
      out << ',';
    first = false;
    out << "{\"kind\":" << uint32_t(c.kind) << ",\"points\":[";
    bool pfirst = true;
    for (auto p : c.points)
    {
      if (!pfirst)
        out << ',';
      pfirst = false;
      out << '[' << p.x << ',' << height(p) << ',' << p.y << ']';
    }
    out << "]}";
  }
  out << "]}";
}
} // namespace
int main(int argc, char **argv)
{
  int failures = 0;
  const auto check = [&](bool okay, const char *text)
  {
    std::cout << (okay ? "PASS " : "FAIL ") << text << '\n';
    failures += !okay;
  };
  const auto valid = [](Point2 p)
  {
    return p.x >= 0 && p.y >= 0 && p.x <= 8000 && p.y <= 8000;
  };
  const HeightFunction plane = [](Point2 p)
  {
    return 100 + .04 * p.x + .02 * p.y;
  };
  TerrainMeshSettings settings;
  settings.tileSizeMetres = 4000;
  settings.workerCount = 2;
  settings.poissonSpacingMetres = 180;
  settings.poissonLayers = 1;
  settings.maximumRefinementRounds = 3;
  settings.preserveChainVertices = true;
  settings.verticalToleranceMetres = 2;
  const MaterialFunction mat = [](Point2)
  {
    return uint8_t(0);
  };
  TerrainMesh dense, simple;
  std::string error;
  check(buildTerrainMesh(border(), plane, mat, 8000, settings, dense, &error), "dense inclined plane builds");
  settings.simplificationRounds = 6;
  settings.simplificationToleranceMetres = .5;
  check(buildTerrainMesh(border(), plane, mat, 8000, settings, simple, &error), "planar simplification builds");
  check(simple.triangleCount() < dense.triangleCount() * .7 && simple.statistics.simplifiedVertices > 0, "inclined planar regions lose at least 30 percent of triangles");
  bool exact = true, seams = true;
  for (size_t t = 0; t < simple.tiles.size(); ++t)
  {
    const auto &tile = simple.tiles[t];
    for (const auto &v : tile.vertices)
      exact &= std::abs(v.y - plane({v.x, v.z})) < 1e-8;
    for (int s = 0; s < 4; ++s)
    {
      const auto &a = tile.borders[s], &b = dense.tiles[t].borders[s];
      seams &= a.size() == b.size();
      for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        seams &= distance(a[i], b[i]) < 1e-9;
    }
  }
  check(exact && seams, "plane and every tile border survive simplification exactly");
  const HeightFunction hill = [](Point2 p)
  {
    const double crest = 5400 + 240 * std::sin(p.x / 1300), river = 1800 + 180 * std::sin(p.x / 1500);
    return 100 + .025 * p.x + 450 * std::exp(-std::pow((p.y - crest) / 500, 2)) - 65 * std::exp(-std::pow((p.y - river) / 95, 2));
  };
  auto ridges = ridgeSurfaceGuides({0, 5400}, {8000, 5400}, 1800, 150, hill, valid);
  double crestError = 0;
  size_t crestCount = 0;
  for (const auto &c : ridges)
    if (c.kind == ChainKind::Ridge)
      for (auto p : c.points)
        if (p.x > 500 && p.x < 7500)
        {
          crestError += std::abs(p.y - (5400 + 240 * std::sin(p.x / 1300)));
          ++crestCount;
        }
  check(crestCount > 20 && crestError / crestCount < 40, "ridge guides follow the shifted final crest");
  std::vector<CorridorGuide> corridors;
  for (int i = 0; i < 32; ++i)
  {
    const double a = i * 250., b = (i + 1) * 250.;
    corridors.push_back({{a, 1800 + 180 * std::sin(a / 1500)}, {b, 1800 + 180 * std::sin(b / 1500)}, 45, 350});
  }
  const auto rivers = riverSurfaceGuides(corridors, 400);
  // Each segment emits seven rails plus two cross sections with this spacing.
  check(distance(rivers[0].points.back(), rivers[9].points.front()) < 1e-8, "river-bank guides join exactly at bends");
  auto slopes = slopeSurfaceGuides(8000, 150, 17, hill, valid);
  check(!slopes.empty() && slopeSurfaceGuides(8000, 150, 17, plane, valid).empty(), "curved slopes get downhill guides and planes get none");
  auto guides = border();
  guides.insert(guides.end(), ridges.begin(), ridges.end());
  guides.insert(guides.end(), rivers.begin(), rivers.end());
  guides.insert(guides.end(), slopes.begin(), slopes.end());
  TerrainMesh before, after;
  settings.poissonSpacingMetres = 250;
  settings.simplificationRounds = 0;
  check(buildTerrainMesh(border(), hill, mat, 8000, settings, before, &error), "unguided terrain builds");
  TerrainMesh guidedDense;
  check(buildTerrainMesh(guides, hill, mat, 8000, settings, guidedDense, &error), "guided reference builds");
  settings.simplificationRounds = 6;
  check(buildTerrainMesh(guides, hill, mat, 8000, settings, after, &error), "ridge, river and slope guides triangulate with simplification");
  check(after.statistics.constraintsIncluded == after.statistics.constraintCount, "simplification preserves every guided constraint");
  check(after.statistics.simplifiedVertices > 0, "planar patches simplify alongside protected terrain features");
  using Edge = std::array<double, 4>;
  const auto edgeKey = [](Point2 a, Point2 b)
  {
    if (a.x > b.x || (a.x == b.x && a.y > b.y))
      std::swap(a, b);
    return Edge{a.x, a.y, b.x, b.y};
  };
  std::set<Edge> retained;
  for (const auto &tile : after.tiles)
    for (size_t i = 0; i < tile.indices.size(); i += 3)
      for (int j = 0; j < 3; ++j)
      {
        const auto &a = tile.vertices[tile.indices[i + j]], &b = tile.vertices[tile.indices[i + (j + 1) % 3]];
        retained.insert(edgeKey({a.x, a.z}, {b.x, b.z}));
      }
  bool protectedEdges = true;
  size_t checked = 0;
  for (const auto &tile : guidedDense.tiles)
    for (size_t i = 0; i < tile.indices.size(); i += 3)
      for (int j = 0; j < 3; ++j)
      {
        const auto &va = tile.vertices[tile.indices[i + j]], &vb = tile.vertices[tile.indices[i + (j + 1) % 3]];
        const Point2 a{va.x, va.z}, b{vb.x, vb.z};
        bool guided = false;
        for (const auto &chain : guides)
          for (size_t k = 0; k + 1 < chain.points.size(); ++k)
          {
            const auto p = chain.points[k], q = chain.points[k + 1];
            const double len = distance(p, q);
            if (len < 1e-9)
              continue;
            const auto on = [&](Point2 v)
            {
              return std::abs((q.x - p.x) * (v.y - p.y) - (q.y - p.y) * (v.x - p.x)) / len < 1e-6 && distance(p, v) + distance(v, q) < len + 1e-6;
            };
            guided |= on(a) && on(b);
          }
        if (guided)
        {
          ++checked;
          protectedEdges &= retained.count(edgeKey(a, b)) != 0;
        }
      }
  check(checked > 100 && protectedEdges, "actual output retains guided edges after local cavity replacement");
  TerrainMesh serial;
  settings.workerCount = 1;
  settings.indexConstraints = false;
  uint32_t progressCalls=0;TerrainMeshProgress lastProgress;
  settings.progress=[&](const TerrainMeshProgress &p){++progressCalls;lastProgress=p;};
  check(buildTerrainMesh(guides, hill, mat, 8000, settings, serial, &error), "serial guided build succeeds");
  bool deterministic = serial.tiles.size() == after.tiles.size();
  for (size_t t = 0; t < std::min(serial.tiles.size(), after.tiles.size()); ++t)
  {
    const auto &a = serial.tiles[t], &b = after.tiles[t];
    deterministic &= a.indices == b.indices && a.materials == b.materials && a.vertices.size() == b.vertices.size();
    for (size_t i = 0; i < std::min(a.vertices.size(), b.vertices.size()); ++i)
      deterministic &= a.vertices[i].x == b.vertices[i].x && a.vertices[i].y == b.vertices[i].y && a.vertices[i].z == b.vertices[i].z;
  }
  check(deterministic, "indexed parallel geometry matches the unindexed serial reference exactly");
  check(progressCalls>=3&&lastProgress.completedTiles==serial.tiles.size()&&lastProgress.activeWorkers==0&&lastProgress.triangles==serial.triangleCount(),"progress reports completion and exact output counts");
  std::cout << "plane triangles " << dense.triangleCount() << " -> " << simple.triangleCount() << "; guided " << after.triangleCount() << "; removed " << after.statistics.simplifiedVertices << '\n';
  if (argc > 1)
  {
    dump(std::string(argv[1]) + ".plane-before.json", dense, {}, plane);
    dump(std::string(argv[1]) + ".plane-after.json", simple, {}, plane);
    dump(std::string(argv[1]) + ".before.json", before, {}, hill);
    dump(std::string(argv[1]) + ".after.json", after, guides, hill);
  }
  return failures ? 1 : 0;
}
