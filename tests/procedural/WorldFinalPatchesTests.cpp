#include "json.hpp"
#include "procedural/world/WorldFarmFields.hpp"
#include "procedural/world/WorldFinalPatches.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace procedural::world::rewrite;
using nlohmann::json;
std::vector<Position> rect(double x, double z, double w, double h)
{
  return {{x, 0, z}, {x + w, 0, z}, {x + w, 0, z + h}, {x, 0, z + h}};
}
SurfaceContour contour(uint64_t id, std::vector<Position> loop, uint32_t material, SurfaceRole role = SurfaceRole::Ground, uint32_t layer = 0)
{
  SurfaceContour c;
  c.id = id;
  c.loop = std::move(loop);
  c.material = material;
  c.role = role;
  c.layer = layer;
  return c;
}
json encode(const WorldFinalPatches &p)
{
  json j;
  j["phase"] = 11;
  j["renderOmittedAreaMetres2"] = 0;
  for (auto v : p.mesh.vertices)
    j["mesh"]["vertices"].push_back({v.x, v.y, v.z});
  for (auto t : p.mesh.triangles)
  {
    j["mesh"]["triangles"].push_back(t.vertices);
    j["mesh"]["materials"].push_back(t.material >= 20 ? 5 : t.material);
    j["faceIds"].push_back(t.face);
    j["roles"].push_back(int(t.role));
    j["layers"].push_back(t.layer);
  }
  for (auto e : p.map.edges)
    j["edges"].push_back({{"id", e.id}, {"a", e.a}, {"b", e.b}, {"sources", e.sourceContours}, {"layer", e.layer}});
  for (auto s : p.sources)
    j["sources"].push_back({{"producer", s.producer}, {"contour", s.contour}, {"segment", s.segment}, {"edges", s.edges}});
  for (auto t : p.tiles)
    j["tiles"].push_back({{"x", t.x}, {"z", t.z}, {"faces", t.faces}, {"edges", t.boundaryEdges}, {"triangles", t.triangles}});
  return j;
}
// Separating-axis overlap test independent of the arrangement/CDT implementation.
bool overlaps(const std::array<Position, 3> &a, const std::array<Position, 3> &b)
{
  for (auto *polygon : {&a, &b})
    for (int i = 0; i < 3; ++i)
    {
      auto u = (*polygon)[i], v = (*polygon)[(i + 1) % 3];
      double x = v.z - u.z, z = u.x - v.x, la = INFINITY, ha = -INFINITY, lb = INFINITY, hb = -INFINITY;
      for (auto p : a)
      {
        auto t = p.x * x + p.z * z;
        la = std::min(la, t);
        ha = std::max(ha, t);
      }
      for (auto p : b)
      {
        auto t = p.x * x + p.z * z;
        lb = std::min(lb, t);
        hb = std::max(hb, t);
      }
      if (std::min(ha, hb) - std::max(la, lb) < 1e-7)
        return false;
    }
  return true;
}
int main(int argc, char **argv)
{
  int assertions = 0;
  std::string error;
  auto check = [&](bool ok, const std::string &name)
  {
    if (!ok)
    {
      std::cerr << "FAIL " << name << ": " << error << '\n';
      std::exit(1);
    }
    ++assertions;
    std::cout << "PASS " << name << '\n';
  };
  auto terrain = [](double x, double z)
  {
    return .025 * x + .015 * z;
  };
  SurfaceMapSettings settings;
  settings.maxX = 120;
  settings.maxZ = 80;
  settings.cutsX = {60};
  settings.cutsZ = {40};
  SurfaceRefinement refinement;
  std::vector<std::vector<WorldContourBatch>> scenes;
  scenes.push_back({{1, 0, {contour(1, rect(0, 0, 60, 80), 20), contour(2, rect(60, 0, 60, 80), 21)}}});
  // Four convex pieces form a genuine polygonal land ring around an excluded channel opening.
  scenes.push_back(
      {{1, 0, {contour(1, rect(10, 10, 100, 15), 20), contour(2, rect(10, 55, 100, 15), 20), contour(3, rect(10, 25, 25, 30), 20), contour(4, rect(85, 25, 25, 30), 20)}},
       {2, 1, {contour(1, rect(35, 25, 50, 30), 9, SurfaceRole::RiverBed)}}});
  scenes.push_back(
      {{1, 0, {contour(1, rect(8, 8, 104, 64), 20)}},
       {2, 1, {contour(1, {{0, 0, 30}, {120, 0, 47}, {120, 0, 49}, {0, 0, 32}}, 1, SurfaceRole::Road)}},
       {3, 2, {contour(1, {{45, 0, 0}, {52, 0, 0}, {79, 0, 80}, {67, 0, 80}}, 9, SurfaceRole::RiverBed)}},
       // Bridge is an independent layer; underlying channel owns the ground opening.
       {4, 3, {contour(1, {{52, 0, 37.3666666667}, {70, 0, 39.9166666667}, {70, 0, 41.9166666667}, {52, 0, 39.3666666667}}, 1, SurfaceRole::Bridge, 1)}}});
  scenes.push_back({{7, 0, {contour(1, rect(20, 10, 80, 60), 20)}}, {8, 1, {contour(1, rect(20, 10, 80, 60), 21)}}});
  // Real field generator output, including its ownership masks and full-width tracks.
  FarmFieldInput field;
  field.outer = rect(5, 5, 110, 70);
  field.access = {{5, 0, 40}, {115, 0, 40}};
  FarmFieldProduct fields;
  check(generateFarmFields(field, farmFieldPreset(FieldRecipe::Enclosed), fields, &error), "field producer integration");
  scenes.push_back({{23, 0, fields.contours}});
  // Two tributary masks join a wider downstream channel. Hydraulics are upstream.
  scenes.push_back(
      {{31,
        0,
        {contour(1, {{10, 0, 0}, {16, 0, 0}, {66, 0, 40}, {58, 0, 40}}, 9, SurfaceRole::RiverBed),
         contour(2, {{99, 0, 0}, {106, 0, 0}, {66, 0, 40}, {58, 0, 40}}, 9, SurfaceRole::RiverBed),
         contour(3, {{58, 0, 40}, {66, 0, 40}, {78, 0, 80}, {62, 0, 80}}, 9, SurfaceRole::RiverBed)}}});
  auto &bridge = scenes[2][3].contours.front();
  bridge.terrainSurface = false;
  bridge.plane = {.025, .015, 3};
  json evidence;
  std::vector<json> exports;
  for (size_t scene = 0; scene < scenes.size(); ++scene)
  {
    WorldFinalPatches p;
    check(freezeWorldPatches(scenes[scene], settings, refinement, terrain, 14, p, &error), "freeze scene " + std::to_string(scene));
    check(auditWorldFinalPatches(p, &error), "100 percent constraint provenance");
    check(p.tiles.size() == 4, "four tile partition");
    double area = 0;
    bool positive = true, ownership = true;
    for (auto t : p.mesh.triangles)
    {
      auto a = p.mesh.vertices[t.vertices[0]], b = p.mesh.vertices[t.vertices[1]], c = p.mesh.vertices[t.vertices[2]];
      double signedArea = ((b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z)) * .5;
      positive &= signedArea > 0;
      if (!t.layer)
        area += signedArea;
      if (scene == 1 && !t.layer)
      {
        double x = (a.x + b.x + c.x) / 3, z = (a.z + b.z + c.z) / 3;
        if (x > 35 && x < 85 && z > 25 && z < 55)
          ownership &= t.role == SurfaceRole::RiverBed;
      }
    }
    check(positive && std::abs(area - 9600) < 1e-5, "independent positive area and coverage");
    check(ownership, "land excludes channel opening");
    bool labels = true;
    for (const auto &t : p.mesh.triangles)
    {
      Position center{};
      for (auto v : t.vertices)
      {
        center.x += p.mesh.vertices[v].x / 3;
        center.z += p.mesh.vertices[v].z / 3;
      }
      uint32_t expected = 0;
      for (const auto &batch : scenes[scene])
        for (const auto &c : batch.contours)
        {
          if (c.layer != t.layer)
            continue;
          bool inside = true;
          for (size_t k = 0; k < c.loop.size(); ++k)
          {
            auto a = c.loop[k], b = c.loop[(k + 1) % c.loop.size()];
            inside &= (b.x - a.x) * (center.z - a.z) - (b.z - a.z) * (center.x - a.x) >= -1e-8;
          }
          if (inside)
            expected = c.material;
        }
      labels &= expected == t.material;
    }
    check(labels, "independent ownership masks exclude roads and channels from land");
    bool disjoint = true;
    for (size_t i = 0; i < p.mesh.triangles.size(); ++i)
      for (size_t k = i + 1; k < p.mesh.triangles.size(); ++k)
      {
        auto a = p.mesh.triangles[i], b = p.mesh.triangles[k];
        if (a.layer != b.layer)
          continue;
        disjoint &=
            !overlaps({p.mesh.vertices[a.vertices[0]], p.mesh.vertices[a.vertices[1]], p.mesh.vertices[a.vertices[2]]}, {p.mesh.vertices[b.vertices[0]], p.mesh.vertices[b.vertices[1]], p.mesh.vertices[b.vertices[2]]});
      }
    check(disjoint, "independent same-layer triangle overlap test");
    auto reordered = scenes[scene];
    std::reverse(reordered.begin(), reordered.end());
    auto cuts = settings;
    cuts.cutsX = {60, 60};
    WorldFinalPatches replay;
    check(freezeWorldPatches(reordered, cuts, refinement, terrain, 14, replay, &error), "worker order replay");
    check(encode(replay) == encode(p), "byte identical seams and mesh after producer reorder");
    auto corrupt = p;
    corrupt.mesh.patches.front().constraints.clear();
    check(!auditWorldFinalPatches(corrupt, &error), "missing CDT constraint fails closed");
    corrupt = p;
    corrupt.mesh.vertices.front().x += .01;
    check(!auditWorldFinalPatches(corrupt, &error), "published vertex corruption rejected");
    corrupt = p;
    corrupt.sources.front().edges.clear();
    check(!auditWorldFinalPatches(corrupt, &error), "missing source chain fails closed");
    corrupt = p;
    corrupt.tiles.front().triangles.clear();
    check(!auditWorldFinalPatches(corrupt, &error), "missing tile coverage fails closed");
    corrupt = p;
    corrupt.tiles.front().boundaryEdges.clear();
    check(!auditWorldFinalPatches(corrupt, &error), "missing seam manifest fails closed");
    corrupt = p;
    auto boundary = corrupt.mesh.patches.front().constraints.front().a;
    corrupt.mesh.patches.front().vertices[boundary].position.x += .01;
    check(!auditWorldFinalPatches(corrupt, &error), "shifted boundary fails closed");
    auto failed = p;
    auto bad = settings;
    bad.cutsX = {-1};
    check(!freezeWorldPatches(scenes[scene], bad, refinement, terrain, 14, failed, &error) && encode(failed) == encode(p), "invalid build preserves published output");
    auto brokenMap = p.map;
    brokenMap.edges.pop_back();
    auto meshBefore = p.mesh;
    check(!meshSurfaceArrangement(brokenMap, terrain, refinement, meshBefore, &error), "missing canonical insertion fails meshing");
    auto exhausted = refinement;
    exhausted.maximumInteriorPoints = 0;
    check(!freezeWorldPatches(scenes[scene], settings, exhausted, terrain, 14, failed, &error) && encode(failed) == encode(p), "meshing failure preserves published output");
    exports.push_back(encode(p));
    evidence["scenes"].push_back({{"scene", scene}, {"sources", p.sources.size()}, {"edges", p.map.edges.size()}, {"patches", p.mesh.patches.size()}, {"triangles", p.mesh.triangles.size()}, {"area", area}});
  }
  WorldFinalPatches unused;
  auto duplicate = scenes[0];
  duplicate.push_back(duplicate.front());
  check(!freezeWorldPatches(duplicate, settings, refinement, terrain, 14, unused, &error), "duplicate producer rejected");
  auto bad = settings;
  bad.cutsZ = {NAN};
  check(!freezeWorldPatches(scenes[0], bad, refinement, terrain, 14, unused, &error), "nonfinite seam rejected");
  evidence["assertions"] = assertions;
  evidence["passed"] = true;
  if (argc > 1)
  {
    std::filesystem::create_directories(argv[1]);
    for (size_t i = 0; i < exports.size(); ++i)
      std::ofstream(std::filesystem::path(argv[1]) / ("scene-" + std::to_string(i) + "-mesh.json")) << exports[i].dump(2);
    std::ofstream(std::filesystem::path(argv[1]) / "validation.json") << evidence.dump(2);
  }
  std::cout << "PASS Step14 " << assertions << " assertions\n";
}
