#include "json.hpp"
#include "procedural/world/WorldRuralPatches.hpp"
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
bool interiorOverlap(const std::vector<Position> &a, const std::vector<Position> &b)
{
  for (auto *p : {&a, &b})
    for (size_t i = 0; i < p->size(); ++i)
    {
      auto u = (*p)[i], v = (*p)[(i + 1) % p->size()];
      double x = v.z - u.z, z = u.x - v.x, loA = INFINITY, hiA = -INFINITY, loB = INFINITY, hiB = -INFINITY;
      for (auto q : a)
      {
        auto t = q.x * x + q.z * z;
        loA = std::min(loA, t);
        hiA = std::max(hiA, t);
      }
      for (auto q : b)
      {
        auto t = q.x * x + q.z * z;
        loB = std::min(loB, t);
        hiB = std::max(hiB, t);
      }
      if (std::min(hiA, hiB) - std::max(loA, loB) <= 1e-7)
        return false;
    }
  return true;
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
  auto flat = [](double, double)
  {
    return 0.;
  };
  WorldSplineLandforms forms;
  WorldPhysicalTerrain physical;
  WorldRiverChannels rivers;
  check(forms.build({}, {}, &error) && physical.build(forms, flat, {}, {}, {}, &error), "physical terrain dependencies");
  SuitabilitySettings ss;
  ss.maxX = 400;
  ss.maxZ = 320;
  ss.spacingMetres = 8;
  RuralPatchSettings settings;
  settings.maximumArea = 6000;
  settings.minimumWidth = 24;
  settings.patchSpacing = 16;
  std::vector<RuralSource> sources = {{10, {44, 0, 156}, 18000}, {20, {356, 0, 156}, 18000}};
  std::vector<Position> exits;
  for (auto s : sources)
    exits.push_back(s.exit);
  auto soil = [](double x, double z)
  {
    return .6 + .2 * std::sin(x * .023) * std::cos(z * .026);
  };
  WorldSuitability world;
  check(
      world.build(
          physical,
          rivers,
          soil,
          [](double, double)
          {
            return .4;
          },
          exits,
          ss,
          12,
          &error),
      "frozen terrain suitability");
  RuralPatchProduct baseline;
  for (int scene = 0; scene < 5; ++scene)
  {
    std::vector<SurfaceContour> reservations;
    auto add = [&](std::vector<Position> p, uint32_t material)
    {
      SurfaceContour c;
      c.id = reservations.size() + 1;
      c.loop = p;
      c.material = material;
      reservations.push_back(c);
    };
    add(rect(0, 124, 32, 64), 11);
    add(rect(368, 124, 32, 64), 11);
    if (scene == 1 || scene == 2)
    {
      for (int i = 0; i < 16; ++i)
      {
        double a = 190 + 12 * std::sin(i * .4), b = 190 + 12 * std::sin((i + 1) * .4);
        add({{a, 0, i * 20.}, {a + 16, 0, i * 20.}, {b + 16, 0, (i + 1) * 20.}, {b, 0, (i + 1) * 20.}}, 9);
      }
    }
    if (scene == 3)
      add(rect(72, 0, 40, 320), 3);
    if (scene == 4)
    {
      add(rect(56, 0, 288, 320), 3);
      add(rect(0, 0, 32, 320), 3);
    }
    auto active = sources;
    if (scene == 1 || scene == 3 || scene == 4)
      active.resize(1);
    RuralPatchProduct result;
    bool ok = growRuralPatches(world, reservations, active, settings, result, &error);
    check(ok, "scene " + std::to_string(scene) + " rural growth: " + error);
    if (!ok)
      continue;
    if (scene == 0)
      baseline = result;
    if (scene == 4)
      check(result.patches.empty() && result.budgets[0].shortfall == active[0].areaBudget, "narrow/blocked domain reports full shortfall without fallback ring");
    else if (scene != 3)
      check(result.patches.size() >= 2, "multiple broad agricultural patches");
    bool access = true, budget = true, eligible = true;
    for (auto &p : result.patches)
    {
      access &= p.access.size() >= 2 && p.accessCost <= settings.maximumAccessCost;
      for (auto i : p.accessCells)
        access &= std::isfinite(result.travelCost[i]) && world.cells()[i].walkable && result.catchments[i] == p.source;
      for (auto i : p.cells)
        eligible &= result.eligible[i] && result.catchments[i] == p.source;
    }
    for (auto b : result.budgets)
      budget &= b.allocated <= b.requested + 1e-6 && std::abs(b.allocated + b.shortfall - b.requested) < 1e-6;
    check(access && eligible, "every patch has same-catchment passable access and eligible support");
    check(budget && std::abs(result.patchArea + result.unallocatedArea - result.eligibleArea) < 1e-6, "demand and eligible-area ledgers balance");
    bool geometrySafe = true, patchDisjoint = true;
    for (size_t i = 0; i < result.contours.size(); ++i)
    {
      auto &c = result.contours[i];
      for (auto &ex : reservations)
        geometrySafe &= !interiorOverlap(c.loop, ex.loop);
      if (c.material >= 20)
        for (uint32_t j = 0; j < world.cells().size(); ++j)
          if (!result.eligible[j])
          {
            auto v = world.cells()[j].center;
            geometrySafe &= !interiorOverlap(c.loop, rect(v.x - 4, v.z - 4, 8, 8));
          }
      if (c.material >= 20)
        for (size_t j = 0; j < i; ++j)
          if (result.contours[j].material >= 20 && result.contours[j].material != c.material)
            patchDisjoint &= !interiorOverlap(c.loop, result.contours[j].loop);
    }
    check(geometrySafe && patchDisjoint, "exported patch and track polygons respect exclusions and patch ownership");
    if (scene == 1 || scene == 3)
    {
      bool side = true;
      for (auto &p : result.patches)
        for (auto v : p.boundary)
          side &= v.x <= (scene == 1 ? 180 : 72);
      check(side, "unbridged river/cliff blocks far-side allocation");
    }
    if (scene == 2)
    {
      bool both = false;
      for (auto &p : result.patches)
        both |= p.source == 20;
      check(both, "independent rural exit serves far catchment without crossing water");
    }
    if (argc > 1)
    {
      std::filesystem::path out = argv[1];
      std::filesystem::create_directories(out);
      json report;
      report["scene"] = scene;
      report["columns"] = world.columns();
      report["rows"] = world.rows();
      report["spacing"] = ss.spacingMetres;
      report["travel"] = json::array();
      report["eligible"] = result.eligible;
      report["patches"] = json::array();
      report["reservations"] = json::array();
      auto poly = [](auto p)
      {
        json a = json::array();
        for (auto v : p)
          a.push_back({v.x, v.y, v.z});
        return a;
      };
      for (auto v : result.travelCost)
        report["travel"].push_back(std::isfinite(v) ? v : -1);
      for (auto &p : result.patches)
        report["patches"].push_back({{"id", p.id}, {"source", p.source}, {"boundary", poly(p.boundary)}, {"access", poly(p.access)}, {"cells", p.cells}, {"area", p.area}});
      for (auto c : reservations)
        report["reservations"].push_back({{"material", c.material}, {"boundary", poly(c.loop)}});
      std::ofstream(out / ("scene-" + std::to_string(scene) + ".json")) << report.dump();
      if (scene < 3)
      {
        auto contours = result.contours;
        uint64_t next = contours.size() + 1;
        for (auto c : reservations)
        {
          c.id = next++;
          contours.push_back(c);
        }
        SurfaceMapSettings bounds;
        bounds.maxX = 400;
        bounds.maxZ = 320;
        SurfaceArrangement map;
        SurfaceArrangementMesh mesh;
        SurfaceRefinement refine;
        refine.groundSpacingMetres = 16;
        refine.modifiedSpacingMetres = 8;
        bool mapped = arrangeSurfaceContours(contours, bounds, flat, 12, map, &error) && meshSurfaceArrangement(map, flat, refine, mesh, &error);
        check(mapped, "scene final contours and CDT: " + error);
        if (mapped)
        {
          check(std::abs(mesh.groundArea - 128000) < .01, "CDT preserves complete rural map area");
          json packed;
          packed["vertices"] = poly(mesh.vertices);
          packed["triangles"] = json::array();
          packed["materials"] = json::array();
          double omitted = 0;
          for (auto t : mesh.triangles)
          {
            auto a = mesh.vertices[t.vertices[0]], b = mesh.vertices[t.vertices[1]], c = mesh.vertices[t.vertices[2]];
            float y = (float(b.z) - float(a.z)) * (float(c.x) - float(a.x)) - (float(b.x) - float(a.x)) * (float(c.z) - float(a.z));
            if (y <= 1e-8f)
            {
              omitted += std::abs((b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z)) * .5;
              continue;
            }
            packed["triangles"].push_back(t.vertices);
            packed["materials"].push_back(t.material >= 20 ? 5 : t.material);
          }
          std::ofstream(out / ("scene-" + std::to_string(scene) + "-mesh.json")) << json{{"phase", 11}, {"mesh", packed}, {"renderOmittedAreaMetres2", omitted}}.dump();
          check(omitted < .0001, "native float packing omits less than 0.0001 square metres");
        }
      }
    }
  }
  SplineFeature plateau;
  plateau.id = 1;
  plateau.heightMetres = 8;
  plateau.faceWidthMetres = 0;
  plateau.weatheredWidthMetres = plateau.apronWidthMetres = 2;
  for (int k = 0; k < 16; ++k)
  {
    double t = k * 6.283185307179586 / 16;
    plateau.controls.push_back({260 + 80 * std::cos(t), 0, 160 + 80 * std::sin(t)});
  }
  WorldSplineLandforms plateauForms;
  WorldPhysicalTerrain cap, closedCap;
  AccessRamp ramp;
  ramp.id = 1;
  ramp.toFeature = 1;
  ramp.halfWidthMetres = 12;
  ramp.shoulderMetres = 4;
  ramp.stations = {{116, 0, 156}, {236, 8, 156}};
  bool capOK = plateauForms.build({plateau}, {}, &error) && cap.build(plateauForms, flat, {ramp}, {}, {}, &error) && closedCap.build(plateauForms, flat, {}, {}, {}, &error);
  check(capOK, "spline plateau and engineered access ramp build");
  auto capSoil = [](double x, double z)
  {
    return std::hypot(x - 260, z - 160) < 62 ? .95 : .05;
  };
  for (int opened = 0; opened < 2; ++opened)
  {
    WorldSuitability cw;
    RuralPatchProduct cp;
    bool built = capOK &&
                 cw.build(
                     opened ? cap : closedCap,
                     rivers,
                     capSoil,
                     [](double, double)
                     {
                       return .4;
                     },
                     {sources[0].exit},
                     ss,
                     13 + opened,
                     &error) &&
                 growRuralPatches(cw, {}, {sources[0]}, settings, cp, &error);
    check(built, "plateau suitability and patch generation: " + error);
    if (built)
      check(opened ? !cp.patches.empty() : cp.patches.empty(), opened ? "usable plateau receives reachable agricultural patch through ramp" : "unramped fertile plateau remains uncultivated");
    if (built && opened && argc > 1)
    {
      auto poly = [](auto p)
      {
        json a = json::array();
        for (auto v : p)
          a.push_back({v.x, v.y, v.z});
        return a;
      };
      json report;
      report["scene"] = 5;
      report["columns"] = cw.columns();
      report["rows"] = cw.rows();
      report["spacing"] = ss.spacingMetres;
      report["eligible"] = cp.eligible;
      report["travel"] = json::array();
      report["patches"] = json::array();
      report["reservations"] = json::array();
      report["plateau"] = poly(plateau.controls);
      for (auto v : cp.travelCost)
        report["travel"].push_back(std::isfinite(v) ? v : -1);
      for (auto &p : cp.patches)
        report["patches"].push_back({{"id", p.id}, {"source", p.source}, {"boundary", poly(p.boundary)}, {"access", poly(p.access)}, {"cells", p.cells}, {"area", p.area}});
      std::ofstream(std::filesystem::path(argv[1]) / "scene-5.json") << report.dump();
    }
  }
  auto reverse = sources;
  std::reverse(reverse.begin(), reverse.end());
  std::vector<SurfaceContour> reserved;
  SurfaceContour a;
  a.id = 1;
  a.material = 11;
  a.loop = rect(0, 124, 32, 64);
  reserved.push_back(a);
  a.id = 2;
  a.loop = rect(368, 124, 32, 64);
  reserved.push_back(a);
  RuralPatchProduct shuffled;
  check(
      growRuralPatches(world, reserved, reverse, settings, shuffled, &error) && shuffled.catchments == baseline.catchments && shuffled.patchArea == baseline.patchArea &&
          shuffled.contours.size() == baseline.contours.size(),
      "source permutation preserves competition and acreage");
  auto offcenter = sources;
  offcenter[0].exit.x += .5;
  RuralPatchProduct offset;
  check(growRuralPatches(world, reserved, offcenter, settings, offset, &error) && offset.travelCost[world.cellAt(offcenter[0].exit.x, offcenter[0].exit.z)] >= .5, "travel ledger includes the subcell exit connection");
  offcenter[0].exit.x = 40.1;
  check(!growRuralPatches(world, reserved, offcenter, settings, offset, &error), "exit without full track width rejects");
  auto imported = settings;
  imported.maximumArea = 30000;
  imported.regions = {{1, rect(64, 16, 96, 96), RuralLandCover::Open}, {2, rect(160, 16, 96, 96), RuralLandCover::Open}, {3, rect(272, 16, 96, 96), RuralLandCover::Forest}};
  auto funded = sources;
  for (auto &s : funded)
    s.areaBudget = 100000;
  RuralPatchProduct mosaic;
  check(
      growRuralPatches(world, {}, funded, imported, mosaic, &error) && mosaic.patches.size() == 2 && mosaic.patchArea == 18432 && mosaic.patches[0].originalRegion == 1,
      "adjoining original cells retain full acreage and forest is excluded");
  std::reverse(imported.regions.begin(), imported.regions.end());
  RuralPatchProduct reordered;
  check(
      growRuralPatches(world, {}, funded, imported, reordered, &error) && reordered.patchArea == mosaic.patchArea && reordered.patches[0].originalRegion == mosaic.patches[0].originalRegion,
      "original region permutation preserves allocation");
  imported.regions.push_back(imported.regions.back());
  check(!growRuralPatches(world, {}, funded, imported, reordered, &error) && reordered.patchArea == mosaic.patchArea, "duplicate source region rejects atomically");
  auto bad = settings;
  bad.minimumWidth = -1;
  auto old = shuffled.patchArea;
  check(!growRuralPatches(world, reserved, sources, bad, shuffled, &error) && shuffled.patchArea == old, "invalid settings reject atomically");
  std::cout << "assertions=" << checks << " failures=" << failures << std::endl;
  return failures ? 1 : 0;
}
