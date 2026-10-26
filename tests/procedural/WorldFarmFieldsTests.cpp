#include "json.hpp"
#include "procedural/world/WorldFarmFields.hpp"
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
double area(const std::vector<Position> &p)
{
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
    a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x;
  return a * .5;
}
bool overlap(const std::vector<Position> &a, const std::vector<Position> &b)
{
  for (auto *p : {&a, &b})
    for (size_t i = 0; i < p->size(); ++i)
    {
      auto u = (*p)[i], v = (*p)[(i + 1) % p->size()];
      double x = v.z - u.z, z = u.x - v.x, la = INFINITY, ha = -INFINITY, lb = INFINITY, hb = -INFINITY;
      for (auto q : a)
      {
        auto t = q.x * x + q.z * z;
        la = std::min(la, t);
        ha = std::max(ha, t);
      }
      for (auto q : b)
      {
        auto t = q.x * x + q.z * z;
        lb = std::min(lb, t);
        hb = std::max(hb, t);
      }
      if (std::min(ha, hb) - std::max(la, lb) <= 1e-6)
        return false;
    }
  return true;
}
std::vector<Position> clipTest(std::vector<Position> p, const std::vector<Position> &mask)
{
  for (size_t i = 0; i < mask.size(); ++i)
  {
    auto a = mask[i], b = mask[(i + 1) % mask.size()];
    auto side = [&](Position v)
    {
      return (b.x - a.x) * (v.z - a.z) - (b.z - a.z) * (v.x - a.x);
    };
    std::vector<Position> next;
    for (size_t j = 0; j < p.size(); ++j)
    {
      auto u = p[j], v = p[(j + 1) % p.size()];
      double x = side(u), y = side(v);
      if (x >= 0)
        next.push_back(u);
      if ((x < 0) != (y < 0))
      {
        double t = x / (x - y);
        next.push_back({u.x + (v.x - u.x) * t, 0, u.z + (v.z - u.z) * t});
      }
    }
    p = std::move(next);
  }
  return p;
}
std::vector<Position> accessBand(Position a, Position b, double width)
{
  double l = std::hypot(b.x - a.x, b.z - a.z), x = -(b.z - a.z) * width / (2 * l), z = (b.x - a.x) * width / (2 * l);
  return {{a.x - x, 0, a.z - z}, {b.x - x, 0, b.z - z}, {b.x + x, 0, b.z + z}, {a.x + x, 0, a.z + z}};
}
int main(int argc, char **argv)
{
  int checks = 0, failures = 0;
  auto check = [&](bool b, const std::string &s)
  {
    ++checks;
    failures += !b;
    std::cout << (b ? "PASS " : "FAIL ") << s << std::endl;
  };
  std::string error;
  FarmFieldProduct first;
  for (int scene = 0; scene < (argc > 2 ? 8 : 7); ++scene)
  {
    FarmFieldInput in;
    in.outer = rect(0, 0, 160, 96);
    in.access = {{-8, 0, 48}, {160, 0, 48}};
    FarmFieldSettings settings;
    if (scene == 1)
    {
      in.outer = {{0, 0, 0}, {160, 0, 0}, {160, 0, 56}, {100, 0, 56}, {100, 0, 96}, {0, 0, 96}};
      auto hole = rect(40, 64, 18, 16);
      std::reverse(hole.begin(), hole.end());
      in.holes = {hole};
      in.access = {{-8, 0, 24}, {160, 0, 24}};
    }
    if (scene == 2)
    {
      in.outer = {{0, 0, 0}, {120, 0, 0}, {18, 0, 90}};
      in.access = {{0, 0, 8}, {105, 0, 8}};
    }
    if (scene == 3)
    {
      in.outer = {{0, 0, 0}, {50, 0, 0}, {50, 0, 42}, {110, 0, 42}, {110, 0, 0}, {160, 0, 0}, {160, 0, 96}, {110, 0, 96}, {110, 0, 43.5}, {50, 0, 43.5}, {50, 0, 96}, {0, 0, 96}};
      in.access = {{-8, 0, 24}, {45, 0, 24}};
    }
    if (scene == 4)
    {
      in.outer = {{0, 0, 0}, {160, 0, 0}, {160, 0, 64}, {120, 0, 96}, {0, 0, 96}};
    }
    if (scene == 5)
    {
      in.outer = rect(0, 0, 140, 22);
      in.access = {{-8, 0, 3}, {140, 0, 3}};
      settings = farmFieldPreset(FieldRecipe::Strips);
      settings.minimumWidth = 4;
      settings.maximumAspect = 32;
      settings.targetArea = 1600;
      settings.maximumArea = 2400;
    }
    if (scene == 6)
    {
      settings.maximumDepth = 0;
      settings.maximumAttempts = 0;
      settings.maximumArea = settings.targetArea = 150;
    }
    if (scene == 7)
    {
      std::ifstream file(argv[2]);
      json data;
      file >> data;
      auto j = data["patches"][0];
      AgriculturalPatch patch;
      patch.id = j["id"];
      patch.source = j["source"];
      for (auto v : j["boundary"])
        patch.boundary.push_back({v[0], v[1], v[2]});
      for (auto v : j["access"])
        patch.access.push_back({v[0], v[1], v[2]});
      in = farmFieldInput(patch, 12);
    }
    FarmFieldProduct result;
    bool ok = generateFarmFields(in, settings, result, &error);
    check(ok, "scene " + std::to_string(scene) + " field compiler: " + error);
    if (!ok)
      continue;
    if (scene == 0)
      first = result;
    if (scene == 6)
      check(result.fields.empty() && result.marginArea > 0, "exhausted split budget retains invalid oversize land as margin");
    else
      check(!result.fields.empty(), "usable field output exists");
    double domain = area(in.outer);
    for (auto h : in.holes)
      domain += area(h);
    check(std::abs(domain - result.patchArea) < 1e-5 && std::abs(result.cropArea + result.marginArea + result.trackArea - domain) < 1e-5, "patch crop track margin ledger closes");
    bool quality = true, disjoint = true;
    double emitted = 0;
    for (auto &f : result.fields)
    {
      quality &= f.area >= settings.minimumArea && f.area <= settings.maximumArea && f.minimumWidth >= settings.minimumWidth - 1e-6 && f.minimumAngleDegrees >= 30 && f.access.size() >= 2;
    }
    auto partition = result.margins;
    partition.insert(partition.end(), result.tracks.begin(), result.tracks.end());
    for (auto &f : result.fields)
      partition.push_back(f.boundary);
    for (size_t i = 0; i < partition.size(); ++i)
    {
      emitted += area(partition[i]);
      for (auto &h : in.holes)
        disjoint &= !overlap(partition[i], h);
      for (size_t j = 0; j < i; ++j)
        disjoint &= !overlap(partition[i], partition[j]);
    }
    check(quality, "terminal area width tip and entrance gates pass");
    check(disjoint && std::abs(emitted - domain) < .01, "emitted geometry partitions domain and preserves holes");
    bool accessGeometry = true;
    auto noncrop = result.margins;
    noncrop.insert(noncrop.end(), result.tracks.begin(), result.tracks.end());
    for (auto &f : result.fields)
      for (size_t i = 1; i < f.access.size(); ++i)
      {
        auto ribbon = accessBand(f.access[i - 1], f.access[i], settings.entranceWidth);
        double covered = 0;
        for (auto &p : noncrop)
          covered += area(clipTest(ribbon, p));
        accessGeometry &= std::abs(covered - area(ribbon)) < 1e-4;
      }
    check(accessGeometry, "full-width entrance ribbons lie in connected noncrop land");
    if (scene == 0)
    {
      FarmFieldProduct repeat;
      bool identical = generateFarmFields(in, settings, repeat, &error) && repeat.fields.size() == result.fields.size();
      if (identical)
        for (size_t i = 0; i < result.fields.size(); ++i)
        {
          auto &a = result.fields[i], &b = repeat.fields[i];
          identical &= a.area == b.area && a.boundary.size() == b.boundary.size();
          if (a.boundary.size() == b.boundary.size())
            for (size_t j = 0; j < a.boundary.size(); ++j)
              identical &= a.boundary[j].x == b.boundary[j].x && a.boundary[j].z == b.boundary[j].z;
        }
      check(identical, "repeated field generation preserves exact boundaries and areas");
    }
    if (scene == 3)
    {
      bool left = true;
      for (auto &f : result.fields)
        for (auto p : f.boundary)
          left &= p.x <= 50;
      check(left, "1.5-metre neck cannot supply full-width access to far fields");
    }
    if (scene == 5)
      check(result.fields.size() == 1 && result.fields[0].aspect > 8, "valid long strip survives separate aspect preset");
    if (argc > 1)
    {
      std::filesystem::path out = argv[1];
      std::filesystem::create_directories(out);
      auto poly = [](auto p)
      {
        json a = json::array();
        for (auto v : p)
          a.push_back({v.x, v.y, v.z});
        return a;
      };
      json report;
      report["scene"] = scene;
      report["outer"] = poly(in.outer);
      report["holes"] = json::array();
      for (auto h : in.holes)
        report["holes"].push_back(poly(h));
      report["fields"] = json::array();
      report["contours"] = json::array();
      report["grain"] = result.grainRadians;
      report["cropArea"] = result.cropArea;
      report["marginArea"] = result.marginArea;
      report["trackArea"] = result.trackArea;
      report["rejections"] = result.rejections;
      for (auto &f : result.fields)
        report["fields"].push_back({{"id", f.id}, {"boundary", poly(f.boundary)}, {"access", poly(f.access)}, {"width", f.minimumWidth}, {"angle", f.minimumAngleDegrees}, {"area", f.area}});
      for (auto &c : result.contours)
        report["contours"].push_back({{"material", c.material}, {"boundary", poly(c.loop)}});
      std::ofstream(out / ("scene-" + std::to_string(scene) + ".json")) << report.dump();
      SurfaceMapSettings bounds;
      bounds.minX = bounds.minZ = -4;
      bounds.maxX = 164;
      bounds.maxZ = 100;
      if (scene == 7)
      {
        bounds.minX = bounds.minZ = 1e30;
        bounds.maxX = bounds.maxZ = -1e30;
        for (auto p : in.outer)
        {
          bounds.minX = std::min(bounds.minX, p.x - 4);
          bounds.minZ = std::min(bounds.minZ, p.z - 4);
          bounds.maxX = std::max(bounds.maxX, p.x + 4);
          bounds.maxZ = std::max(bounds.maxZ, p.z + 4);
        }
      }
      SurfaceArrangement map;
      SurfaceArrangementMesh mesh;
      SurfaceRefinement refine;
      refine.groundSpacingMetres = 10;
      refine.modifiedSpacingMetres = 4;
      auto flat = [](double, double)
      {
        return 0.;
      };
      bool meshed = arrangeSurfaceContours(result.contours, bounds, flat, 1, map, &error) && meshSurfaceArrangement(map, flat, refine, mesh, &error);
      check(meshed, "final field contours feed terrain CDT: " + error);
      if (meshed)
      {
        json m;
        m["vertices"] = poly(mesh.vertices);
        m["triangles"] = json::array();
        m["materials"] = json::array();
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
          m["triangles"].push_back(t.vertices);
          m["materials"].push_back(t.material >= 20 ? 5 : t.material);
        }
        check(omitted < .0001, "float export omission stays below 0.0001 square metres");
        std::ofstream(out / ("scene-" + std::to_string(scene) + "-mesh.json")) << json{{"phase", 11}, {"mesh", m}, {"renderOmittedAreaMetres2", omitted}}.dump();
      }
    }
  }
  FarmFieldInput invalid;
  invalid.outer = rect(0, 0, 160, 96);
  invalid.access = {{0, 0, 48}, {160, 0, 48}};
  FarmFieldSettings bad;
  bad.minimumWidth = -1;
  double saved = first.cropArea;
  check(!generateFarmFields(invalid, bad, first, &error) && first.cropArea == saved, "invalid dimensions reject atomically");
  std::cout << "assertions=" << checks << " failures=" << failures << std::endl;
  return failures ? 1 : 0;
}
