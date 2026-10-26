#include "json.hpp"
#include "procedural/world/WorldSurfaceRefinement.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace procedural::world::rewrite;
using nlohmann::json;
SurfaceContour panel(uint64_t id, double x, double z, double w, double d, uint32_t material, SurfaceRole role, SurfacePlane plane)
{
  SurfaceContour c;
  c.id = id;
  c.loop = {{x, 0, z}, {x + w, 0, z}, {x + w, 0, z + d}, {x, 0, z + d}};
  c.material = material;
  c.role = role;
  c.terrainSurface = false;
  c.plane = plane;
  return c;
}
json encode(const RefinedWorldSurfaces &r, const AssembledSurfaceMesh &m)
{
  json j;
  j["phase"] = 15;
  j["heightErrors"] = r.triangleHeightErrors;
  j["normalErrors"] = r.triangleNormalErrors;
  j["maxHeightError"] = r.maximumHeightError;
  j["maxNormalError"] = r.maximumNormalError;
  j["addedVertices"] = r.addedVertices;
  j["passes"] = r.passes;
  j["openEdges"] = m.openEdges;
  j["sharedEdges"] = m.sharedEdges;
  for (auto v : m.geometry.vertices)
    j["mesh"]["vertices"].push_back({v.x, v.y, v.z});
  for (auto t : m.geometry.triangles)
    j["mesh"]["triangles"].push_back(t);
  for (auto n : m.geometry.expectedNormals)
    j["mesh"]["expectedNormals"].push_back({n.x, n.y, n.z});
  j["mesh"]["materials"] = m.materials;
  return j;
}
int main(int argc, char **argv)
{
  int count = 0;
  std::string error;
  json evidence;
  auto check = [&](bool ok, std::string name)
  {
    if (!ok)
    {
      std::cerr << "FAIL " << name << ": " << error << '\n';
      std::exit(1);
    }
    ++count;
    std::cout << "PASS " << name << '\n';
  };
  SurfaceMapSettings map;
  map.maxX = 24;
  map.maxZ = 20;
  SurfaceRefinement coarse;
  coarse.groundSpacingMetres = 40;
  coarse.modifiedSpacingMetres = 40;
  SurfaceErrorSettings settings;
  settings.heightMetres = .08;
  settings.normalDegrees = 25;
  settings.maximumPasses = 24;
  for (int scene = 0; scene < 5; ++scene)
  {
    std::vector<SurfaceContour> contours;
    std::vector<SurfaceFeatureSample> features;
    SplineFaceMesh structures;
    std::function<double(double, double)> terrain;
    if (scene == 0)
    {
      terrain = [](double x, double z)
      {
        double envelope = std::pow(std::sin(3.141592653589793 * x / 24) * std::sin(3.141592653589793 * z / 20), 2);
        return 3 * std::exp(-std::pow((x - 11.3) / .8, 2)) * envelope;
      };
      for (double z : {3., 7., 10., 13., 17.})
        for (double x : {10.5, 11.3, 12.1})
          features.push_back({1, {x, 0, z}});
    }
    if (scene == 1)
    {
      terrain = [](double x, double)
      {
        return x < 8 ? .3 * x : x < 12 ? 2.4 : x < 16 ? 2.4 + .8 * (x - 12) : .3 * x + .8;
      };
      contours = {
        panel(1, 0, 0, 8, 20, 0, SurfaceRole::Ground, {.3, 0, 0}),
        panel(2, 8, 0, 4, 20, 1, SurfaceRole::Road, {0, 0, 2.4}),
        panel(3, 12, 0, 4, 20, 3, SurfaceRole::Cut, {.8, 0, -7.2}),
        panel(4, 16, 0, 8, 20, 0, SurfaceRole::Ground, {.3, 0, .8})};
    }
    if (scene == 2)
    {
      // Smooth deep channel, pinched upstream and wider downstream; boundary heights vanish.
      terrain = [](double x, double z)
      {
        double w = 1 + z * .07;
        return -4 * std::exp(-std::pow((x - 12) / w, 2)) * std::pow(std::sin(3.141592653589793 * z / 20) * std::sin(3.141592653589793 * x / 24), 2);
      };
      for (double z : {2., 5., 8., 11., 14., 17., 19.})
        for (double x : {10., 12., 14.})
          features.push_back({1, {x, 0, z}});
    }
    if (scene == 3)
      terrain = [](double, double)
      {
        return 5.;
      };
    if (scene == 4)
    {
      terrain = [](double, double)
      {
        return -3.;
      };
      std::vector<std::vector<Position>> loops = {{{2, 0, 0}, {4, 0, 0}, {14, 0, 10}, {10, 0, 10}}, {{20, 0, 0}, {22, 0, 0}, {14, 0, 10}, {10, 0, 10}}, {{10, 0, 10}, {14, 0, 10}, {16, 0, 20}, {8, 0, 20}}};
      uint64_t id = 1;
      for (auto loop : loops)
      {
        SurfaceContour c;
        c.id = id++;
        c.loop = loop;
        c.role = SurfaceRole::RiverBed;
        c.material = 9;
        c.terrainSurface = false;
        c.plane.constant = -3;
        contours.push_back(c);
      }
      for (auto loop : loops)
      {
        SurfaceContour c;
        c.id = id++;
        c.loop = loop;
        c.role = SurfaceRole::Water;
        c.material = 9;
        c.layer = 1;
        c.terrainSurface = false;
        contours.push_back(c);
      }
    }
    WorldFinalPatches frozen;
    check(freezeWorldPatches(contours.empty() ? std::vector<WorldContourBatch>{} : std::vector<WorldContourBatch>{{1, 0, contours}}, map, coarse, terrain, 15, frozen, &error), "freeze scene " + std::to_string(scene));
    PatchSurface source = [&](const SurfaceMapFace &f, double x, double z)
    {
      return f.terrainSurface ? terrain(x, z) : f.plane.height(x, z);
    };
    RefinedWorldSurfaces refined;
    check(refineWorldSurfaces(frozen, source, features, settings, refined, &error), "refine scene " + std::to_string(scene));
    check(refined.maximumHeightError <= settings.heightMetres, "measured height error accepted");
    check(refined.maximumNormalError <= settings.normalDegrees, "measured normal error accepted");
    check(auditWorldFinalPatches(refined.world, &error), "frozen constraints and material ownership retained");
    if (scene == 0 || scene == 2)
      check(refined.addedVertices > features.size(), "adaptive samples added beyond mandatory features");
    // Independent dense barycentric probes, offset from all production probe weights.
    double dense = 0;
    json section = json::array();
    for (auto t : refined.world.mesh.triangles)
    {
      auto a = refined.world.mesh.vertices[t.vertices[0]], b = refined.world.mesh.vertices[t.vertices[1]], c = refined.world.mesh.vertices[t.vertices[2]];
      for (int i = 1; i < 11; ++i)
        for (int k = 1; k < 11 - i; ++k)
        {
          double u = i / 11., v = k / 11., w = 1 - u - v;
          double x = a.x * u + b.x * v + c.x * w, z = a.z * u + b.z * v + c.z * w;
          dense = std::max(dense, std::abs(source(refined.world.map.faces[t.face - 1], x, z) - (a.y * u + b.y * v + c.y * w)));
        }
    }
    check(dense < .14, "independent dense surface error below 14cm");
    bool sectionCovered = true;
    for (int i = 0; i <= 240; ++i)
    {
      double x = i * .1, z = 9.713;
      double meshHeight = NAN;
      for (auto t : refined.world.mesh.triangles)
      {
        auto a = refined.world.mesh.vertices[t.vertices[0]], b = refined.world.mesh.vertices[t.vertices[1]], c = refined.world.mesh.vertices[t.vertices[2]];
        double den = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
        double u = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / den, v = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / den, w = 1 - u - v;
        if (u >= -1e-8 && v >= -1e-8 && w >= -1e-8)
        {
          meshHeight = a.y * u + b.y * v + c.y * w;
          break;
        }
      }
      sectionCovered &= std::isfinite(meshHeight);
      section.push_back({x, terrain(x, z), meshHeight});
    }
    check(sectionCovered, "all 241 cross-section queries covered");
    if (scene == 3)
    {
      // Close the plateau top with vertical faces and a bottom. Reuse every frozen rim station.
      for (auto e : frozen.map.edges)
        if (!e.leftFace || !e.rightFace)
        {
          auto a = frozen.map.vertices[e.a], b = frozen.map.vertices[e.b];
          if (!e.leftFace)
            std::swap(a, b);
          Position lowA{a.x, 0, a.z}, lowB{b.x, 0, b.z};
          uint32_t base = structures.vertices.size();
          structures.vertices.insert(structures.vertices.end(), {a, b, lowA, lowB});
          structures.triangles.push_back({base, base + 1, base + 2});
          structures.triangles.push_back({base + 1, base + 3, base + 2});
          Position expected{b.z - a.z, 0, a.x - b.x};
          structures.expectedNormals.push_back(expected);
          structures.expectedNormals.push_back(expected);
        }
      // Bottom faces match top topology exactly and reverse winding.
      for (auto t : refined.world.mesh.triangles)
      {
        uint32_t base = structures.vertices.size();
        for (auto id : t.vertices)
        {
          auto p = refined.world.mesh.vertices[id];
          p.y = 0;
          structures.vertices.push_back(p);
        }
        structures.triangles.push_back({base, base + 2, base + 1});
        structures.expectedNormals.push_back({0, -1, 0});
      }
    }
    AssembledSurfaceMesh assembled;
    check(assembleSurfaceMesh(refined, structures, 12, scene != 3, assembled, &error), "assemble geometry and hard normals");
    check(scene == 3 ? assembled.openEdges == 0 : assembled.openEdges > 0, "classified closed versus open boundaries");
    if (scene == 1)
    {
      bool crease = false;
      for (auto n : assembled.geometry.expectedNormals)
        crease |= n.x < -.5;
      check(crease, "road cut retains steep normal crease");
    }
    if (scene == 3)
    {
      auto bad = structures;
      bad.triangles.pop_back();
      bad.expectedNormals.pop_back();
      AssembledSurfaceMesh unused;
      check(!assembleSurfaceMesh(refined, bad, 12, false, unused, &error), "missing closed face rejected");
    }
    auto limited = settings;
    limited.maximumAddedVertices = 0;
    RefinedWorldSurfaces failed = refined;
    if (scene == 0 || scene == 2)
      check(!refineWorldSurfaces(frozen, source, features, limited, failed, &error) && failed.world.mesh.triangles.size() == refined.world.mesh.triangles.size(), "exhaustion fails atomically");
    auto malformed = frozen;
    malformed.mesh.patches.front().constraints.clear();
    check(!refineWorldSurfaces(malformed, source, {}, settings, failed, &error), "malformed patch rejected");
    PatchSurface badBoundary = [](const SurfaceMapFace &, double, double)
    {
      return 100.;
    };
    check(!refineWorldSurfaces(frozen, badBoundary, {}, settings, failed, &error), "underresolved frozen boundary rejected");
    if (scene == 4)
    {
      double waterArea = 0;
      bool depth = true;
      for (auto t : refined.world.mesh.triangles)
        if (t.role == SurfaceRole::Water)
        {
          auto a = refined.world.mesh.vertices[t.vertices[0]], b = refined.world.mesh.vertices[t.vertices[1]], c = refined.world.mesh.vertices[t.vertices[2]];
          waterArea += ((b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z)) * .5;
          depth &= a.y == 0 && b.y == 0 && c.y == 0;
        }
      check(depth && std::abs(waterArea - 116) < 1e-8, "unioned confluence water with separate bed depth");
    }
    RefinedWorldSurfaces replay;
    check(refineWorldSurfaces(frozen, source, features, settings, replay, &error), "deterministic refinement replay");
    AssembledSurfaceMesh replayMesh;
    check(assembleSurfaceMesh(replay, structures, 12, scene != 3, replayMesh, &error) && encode(replay, replayMesh) == encode(refined, assembled), "repeat geometry and error reports identical");
    if (scene == 0)
    {
      auto passLimit = settings;
      passLimit.maximumPasses = 1;
      check(!refineWorldSurfaces(frozen, source, features, passLimit, failed, &error), "pass exhaustion is not accepted");
      PatchSurface invalid = [](const SurfaceMapFace &, double, double)
      {
        return NAN;
      };
      check(!refineWorldSurfaces(frozen, invalid, {}, settings, failed, &error), "nonfinite source rejected");
      auto invalidFeature = features;
      invalidFeature.push_back({1, {-10, 0, -10}});
      check(!refineWorldSurfaces(frozen, source, invalidFeature, settings, failed, &error), "outside mandatory sample rejected");
    }
    auto j = encode(refined, assembled);
    j["denseError"] = dense;
    j["section"] = section;
    evidence["scenes"].push_back(
        {{"scene", scene}, {"denseError", dense}, {"probeError", refined.maximumHeightError}, {"added", refined.addedVertices}, {"passes", refined.passes}, {"triangles", assembled.geometry.triangles.size()}});
    if (argc > 1)
    {
      std::filesystem::create_directories(argv[1]);
      std::ofstream(std::filesystem::path(argv[1]) / ("scene-" + std::to_string(scene) + "-mesh.json")) << j.dump(2);
    }
  }
  evidence["assertions"] = count;
  evidence["passed"] = true;
  if (argc > 1)
    std::ofstream(std::filesystem::path(argv[1]) / "validation.json") << evidence.dump(2);
  std::cout << "PASS Step15 " << count << " assertions\n";
}
