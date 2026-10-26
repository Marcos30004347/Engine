#include "json.hpp"
#include "procedural/world/WorldCityLayout.hpp"
#include "procedural/world/WorldRiverChannels.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
using namespace procedural::world::rewrite;
using nlohmann::json;
using Poly = std::vector<Position>;
Poly rect(double x, double z, double w, double h)
{
  return {{x, 0, z}, {x + w, 0, z}, {x + w, 0, z + h}, {x, 0, z + h}};
}
json polygon(const Poly &p)
{
  json a = json::array();
  for (auto v : p)
    a.push_back({v.x, v.y, v.z});
  return a;
}
json encode(const CityLayout &c)
{
  json j;
  j["revision"] = c.revision;
  j["wallSetback"] = c.wallSetback;
  j["acceptedEnvelope"] = polygon(c.envelope.outer);
  j["towers"] = json::array();
  for (const auto &t : c.towers)
    j["towers"].push_back({{"id", t.id}, {"center", polygon({t.center})}, {"footprint", polygon(t.footprint)}, {"height", t.height}});
  j["suburbs"] = json::array();
  for (const auto &p : c.suburbs)
    j["suburbs"].push_back(polygon(p));
  j["districts"] = json::array();
  for (const auto &district : c.districts)
  {
    json pieces = json::array();
    for (const auto &p : district.pieces)
      pieces.push_back(polygon(p));
    j["districts"].push_back({{"id", district.id}, {"use", int(district.use)}, {"name", districtUseName(district.use)}, {"pieces", pieces}, {"area", district.area}, {"access", polygon(district.access)}});
  }
  j["wards"] = json::array();
  j["objects"] = json::array();
  j["streets"] = json::array();
  for (auto &w : c.wards)
  {
    json p = json::array();
    for (auto &q : w.pieces)
      p.push_back(polygon(q));
    j["wards"].push_back({{"id", w.id}, {"pieces", p}});
  }
  for (auto &s : c.streets)
    j["streets"].push_back({{"id", s.id}, {"a", {s.a.x, s.a.y, s.a.z}}, {"b", {s.b.x, s.b.y, s.b.z}}, {"width", s.halfWidth * 2}, {"arterial", s.arterial}, {"hierarchy", s.hierarchy}, {"path", polygon(s.path)}});
  for (auto &o : c.objects)
  {
    json parts = json::array();
    for (const auto &part : o.parts)
      parts.push_back({{"footprint", polygon(part.footprint)}, {"height", part.height}});
    j["objects"].push_back(
        {{"id", o.id},
         {"ward", o.ward},
         {"block", o.block},
         {"outsideWalls", o.outsideWalls},
         {"frontage", o.frontage},
         {"street", o.street},
         {"kind", int(o.kind)},
         {"footprint", polygon(o.footprint)},
         {"clearance", polygon(o.clearance)},
         {"entrance", {o.entrance.x, o.entrance.y, o.entrance.z}},
         {"access", {o.access.x, o.access.y, o.access.z}},
         {"foundation", o.foundation},
         {"height", o.height},
         {"buildingType", int(o.buildingType)},
         {"requestedBuildingType", int(o.requestedBuildingType)},
         {"parts", parts},
         {"courtyard", polygon(o.courtyard)},
         {"compactFallback", o.compactFallback}});
  }
  j["blocks"] = json::array();
  for (const auto &b : c.blocks)
  {
    json courts = json::array(), holes = json::array(), unallocated = json::array();
    for (const auto &p : b.unallocated)
      unallocated.push_back(polygon(p.outer));
    for (const auto &p : b.courtyards)
      courts.push_back(polygon(p.outer));
    for (const auto &p : b.boundary.holes)
      holes.push_back(polygon(p));
    j["blocks"].push_back(
        {{"id", b.id}, {"boundary", polygon(b.boundary.outer)}, {"holes", holes}, {"courtyards", courts}, {"unallocated", unallocated}, {"rejection", b.rejection}, {"district", b.district}, {"use", int(b.use)}});
  }
  j["contours"] = json::array();
  for (const auto &contour : c.contours)
    j["contours"].push_back({{"id", contour.id}, {"material", contour.material}, {"loop", polygon(contour.loop)}});
  j["sites"] = polygon(c.sites);
  j["rejected"] = c.rejectedAreas;
  j["splitAttempts"] = c.splitAttempts;
  j["unrefinedBlocks"] = c.unrefinedBlocks;
  j["subdivisionCells"] = c.subdivisionCells;
  j["frontages"] = json::array();
  for (const auto &f : c.frontages)
    j["frontages"].push_back({{"id", f.id}, {"ward", f.ward}, {"street", f.street}, {"side", f.side}, {"path", polygon(f.path)}});
  return j;
}
int main(int argc, char **argv)
{
  int failures = 0, checks = 0;
  auto check = [&](bool ok, const std::string &name)
  {
    ++checks;
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
    if (!ok)
      ++failures;
  };
  std::filesystem::path output = argc > 1 ? argv[1] : "";
  if (!output.empty())
    std::filesystem::create_directories(output);
  for (int preset = 0; preset < 5; ++preset)
    for (int recipe = 0; recipe < 2; ++recipe)
    {
      CityPolygon domain;
      domain.outer = rect(0, 0, 140, 120);
      if (preset == 1)
      {
        domain.outer = rect(0, 0, 92, 120);
        domain.holes.push_back(rect(64, 66, 15, 35));
      }
      if (preset == 0 || preset == 2)
        domain.outer = {{0, 0, 20}, {25, 0, 0}, {100, 0, 0}, {140, 0, 20}, {140, 0, 65}, {117, 0, 95}, {80, 0, 120}, {25, 0, 108}, {0, 0, 70}};
      if (preset == 4)
        domain.outer = rect(0, 0, 105, 120);
      if (preset == 3)
        domain.outer = {{0, 0, 0}, {140, 0, 0}, {140, 0, 60}, {105, 0, 60}, {105, 0, 120}, {0, 0, 120}};
      std::vector<Poly> reservations; // No implicit central plaza.
      CitySettings s;
      s.seed = 715 + preset;
      s.recipe = CityRecipe(recipe);
      s.siteSpacing = 30;
      s.walls = preset == 2;
      CityLayout c;
      std::string error;
      std::vector<CityStreet> arteries{{1, {0, 0, 28}, {140, 0, 28}, 3, true}};
      std::vector<CityGate> gates{{1, {0, 0, 28}, 1}, {2, {140, 0, 28}, 1}};
      if (preset == 0)
      {
        arteries.push_back({2, {40, 0, 0}, {40, 0, 90}, 2.5, true});
        gates.push_back({3, {40, 0, 0}, 2});
      }
      if (preset == 4)
      {
        arteries[0].b.x = 105;
        gates[1].position.x = 105;
      }
      if (preset == 1)
      {
        arteries[0].b.x = 92;
        gates[1].position.x = 92;
      }
      WorldRiverChannels hydraulic;
      if (preset == 1)
        check(hydraulic.build({{1, {115, -2, 0}, 0}, {2, {115, -1.9, 120}, 0}}, {{2, 1, 1}}, {{1, 2, 45000000, 3.6, 1}}, {}, &error), "river-town hydraulic source");
      auto terrain = [&](double x, double z)
      {
        if (preset == 4)
          return 5. - std::max(0., x - 105) * .15;
        if (preset == 3)
          return -std::min(.6, std::max(0., std::min(x - 105, z - 60)) * .4);
        if (preset == 1)
        {
          ChannelSample value;
          if (!hydraulic.sample(
                  x,
                  z,
                  [](double, double)
                  {
                    return 0.;
                  },
                  value))
            return std::numeric_limits<double>::quiet_NaN();
          double pond = -std::min(.6, std::max(0., std::min({x - 64, 79 - x, z - 66, 101 - z})) * .4);
          return std::min(pond, value.terrainHeight);
        }
        return 0.;
      };
      std::string tag = std::to_string(preset) + "-" + std::to_string(recipe);
      bool generated = generateCityLayout(domain, reservations, arteries, gates, s, terrain, c, &error);
      check(generated, tag + " generate " + error);
      if (!generated)
        continue;
      CityLayout repeat;
      check(generateCityLayout(domain, reservations, arteries, gates, s, terrain, repeat, &error) && encode(c) == encode(repeat), tag + " deterministic objects and topology");
      size_t occupied = 0;
      bool collisions = false, clear = true, access = true;
      for (size_t i = 0; i < c.objects.size(); ++i)
      {
        auto &o = c.objects[i];
        if (o.kind == CityObjectKind::RejectedGap)
          continue;
        ++occupied;
        for (auto &p : reservations)
          if (cityPolygonsOverlap(o.clearance, p))
            clear = false;
        for (auto &p : domain.holes)
          if (cityPolygonsOverlap(o.clearance, p))
            clear = false;
        for (size_t j = 0; j < i; ++j)
          if (cityPolygonsOverlap(o.clearance, c.objects[j].clearance))
            collisions = true;
        bool found = false;
        for (auto &a : c.streets)
        {
          for (size_t k = 1; k < a.path.size(); ++k)
          {
            auto u = a.path[k - 1], v = a.path[k];
            double dx = v.x - u.x, dz = v.z - u.z, l = std::hypot(dx, dz), t = ((o.access.x - u.x) * dx + (o.access.z - u.z) * dz) / (l * l);
            if (t >= -1e-7 && t <= 1 + 1e-7 && std::abs(dx * (o.access.z - u.z) - dz * (o.access.x - u.x)) / l < 1e-4)
              found = true;
          }
        }
        access &= found;
      }
      check(occupied > 20, tag + " populated street frontage");
      check(!collisions, tag + " overhang clearance no collisions");
      check(clear, tag + " water and plaza preserved");
      bool fixedStraight = true, curved = false;
      for (const auto &street : c.streets)
        for (auto point : street.path)
        {
          double deviation = std::abs((street.b.x - street.a.x) * (point.z - street.a.z) - (street.b.z - street.a.z) * (point.x - street.a.x));
          if (street.arterial && deviation > 1e-6)
            fixedStraight = false;
          if (!street.arterial && deviation > 1e-3)
            curved = true;
        }
      check(fixedStraight && curved, tag + " local streets curve while fixed arterial paths remain locked");
      check(access, tag + " occupied objects have street access");
      bool aligned = true, owned = true, passages = true;
      for (const auto &o : c.objects)
      {
        if (o.kind == CityObjectKind::RejectedGap)
          continue;
        const auto front = std::find_if(
            c.frontages.begin(),
            c.frontages.end(),
            [&](const auto &f)
            {
              return f.id == o.frontage;
            });
        owned &= front != c.frontages.end() && front->ward == o.ward && front->street == o.street;
        const double ex = o.entrance.x - o.access.x, ez = o.entrance.z - o.access.z;
        const double tx = o.footprint[1].x - o.footprint[0].x, tz = o.footprint[1].z - o.footprint[0].z;
        aligned &= std::abs(ex * tx + ez * tz) < 1e-6;
        // Independently clip each access segment against every other convex
        // clearance. A point merely lying on a street is insufficient access.
        for (const auto &other : c.objects)
        {
          if (other.id == o.id || other.kind == CityObjectKind::RejectedGap)
            continue;
          double lo = 0, hi = 1;
          for (size_t k = 0; k < other.clearance.size(); ++k)
          {
            const auto a = other.clearance[k], b = other.clearance[(k + 1) % other.clearance.size()];
            auto side = [&](Position p)
            {
              return (b.x - a.x) * (p.z - a.z) - (b.z - a.z) * (p.x - a.x);
            };
            const double da = side(o.entrance), db = side(o.access);
            if (da < 0 && db < 0)
            {
              hi = -1;
              break;
            }
            if (da < 0)
              lo = std::max(lo, da / (da - db));
            if (db < 0)
              hi = std::min(hi, da / (da - db));
          }
          passages &= hi < lo;
        }
      }
      check(aligned && owned, tag + " frontage follows retained street with consistent ownership");
      check(passages, tag + " entrance segments remain unobstructed");
      auto reservationContour = [&](uint64_t id, Poly p, uint32_t material, SurfaceRole role, uint32_t layer, double height)
      {
        SurfaceContour contour;
        contour.id = id;
        contour.loop = p;
        contour.material = material;
        contour.role = role;
        contour.layer = layer;
        contour.terrainSurface = layer == 0;
        contour.plane.constant = height;
        c.contours.push_back(contour);
      };
      auto basinContours = [&](uint64_t id, double x, double z, double w, double h, double inset)
      {
        std::vector<Poly> banks{
          {{x, 0, z}, {x + w, 0, z}, {x + w - inset, 0, z + inset}, {x + inset, 0, z + inset}},
          {{x + w, 0, z}, {x + w, 0, z + h}, {x + w - inset, 0, z + h - inset}, {x + w - inset, 0, z + inset}},
          {{x + w, 0, z + h}, {x, 0, z + h}, {x + inset, 0, z + h - inset}, {x + w - inset, 0, z + h - inset}},
          {{x, 0, z + h}, {x, 0, z}, {x + inset, 0, z + inset}, {x + inset, 0, z + h - inset}}};
        for (size_t k = 0; k < 4; ++k)
          reservationContour(id + k, banks[k], 5, SurfaceRole::RiverBank, 0, 0);
        reservationContour(id + 4, rect(x + inset, z + inset, w - 2 * inset, h - 2 * inset), 6, SurfaceRole::RiverBed, 0, 0);
      };
      if (preset == 1)
      {
        double bottom = hydraulic.reaches()[0].normal.bottomWidth * .5, bank = hydraulic.reaches()[0].normal.waterWidth * .5;
        std::vector<double> x{115 - bank - 8, 115 - bank, 115 - bottom, 115 + bottom, 115 + bank, 115 + bank + 8};
        for (size_t k = 0; k + 1 < x.size(); ++k)
          reservationContour(10000 + k, rect(x[k], 0, x[k + 1] - x[k], 120), k == 2 ? 6 : 5, k == 2 ? SurfaceRole::RiverBed : SurfaceRole::RiverBank, 0, 0);
        reservationContour(10005, rect(115 - bank, 0, bank * 2, 120), 9, SurfaceRole::Water, 1, -2);
        c.contours.back().plane.z = .1 / 120;
        basinContours(10010, 64, 66, 15, 35, 1.5);
        reservationContour(10006, rect(64.25, 66.25, 14.5, 34.5), 9, SurfaceRole::Water, 1, -.1);
        check(std::abs(hydraulic.reaches()[0].discharge - 45) < 1e-9, "river-town retains 45 cubic metres per second");
      }
      if (preset == 3)
      {
        reservationContour(10001, {{105, 0, 60}, {140, 0, 60}, {140, 0, 61.5}, {106.5, 0, 61.5}}, 5, SurfaceRole::RiverBank, 0, 0);
        reservationContour(10002, {{105, 0, 60}, {106.5, 0, 61.5}, {106.5, 0, 120}, {105, 0, 120}}, 5, SurfaceRole::RiverBank, 0, 0);
        reservationContour(10003, rect(106.5, 61.5, 33.5, 58.5), 6, SurfaceRole::RiverBed, 0, 0);
        reservationContour(10000, rect(105.25, 60.25, 34.75, 59.75), 9, SurfaceRole::Water, 1, -.1);
      }
      SurfaceMapSettings settings;
      settings.maxX = 140;
      settings.maxZ = 120;
      settings.maximumFaces = 20000;
      SurfaceArrangement map;
      bool mapped = arrangeSurfaceContours(c.contours, settings, terrain, 11, map, &error);
      check(mapped, tag + " authoritative map " + error);
      SurfaceArrangementMesh mesh;
      if (mapped)
      {
        SurfaceRefinement ref;
        ref.groundSpacingMetres = 20;
        bool meshed = meshSurfaceArrangement(map, terrain, ref, mesh, &error);
        check(meshed, tag + " labeled CDT " + error);
        if (meshed)
          check(std::abs(mesh.groundArea - 16800) < 1e-4, tag + " full terrain coverage");
      }
      std::vector<CityInstanceTriangle> instances;
      check(meshCityInstances(c, terrain, instances, &error) && !instances.empty(), tag + " terrain-seated building massing");
      if (preset == 2)
        check(!c.walls.empty(), tag + " enclosure has open gates");
      if (!output.empty())
      {
        auto j = encode(c);
        j["envelope"] = polygon(domain.outer);
        j["holes"] = json::array();
        for (auto &p : domain.holes)
          j["holes"].push_back(polygon(p));
        j["reservations"] = json::array();
        for (auto &p : reservations)
          j["reservations"].push_back(polygon(p));
        j["triangles"] = mesh.triangles.size();
        j["mapFaces"] = map.faces.size();
        j["walls"] = json::array();
        for (auto &wall : c.walls)
          j["walls"].push_back({polygon({wall.a, wall.b}), wall.halfWidth});
        std::ofstream(output / (tag + ".json")) << j.dump(2);
        json packed;
        packed["phase"] = 11;
        packed["mesh"]["vertices"] = json::array();
        packed["mesh"]["triangles"] = json::array();
        packed["mesh"]["materials"] = json::array();
        packed["mesh"]["expectedNormals"] = json::array();
        uint32_t omitted = 0;
        double omittedArea = 0;
        auto emit = [&](std::array<Position, 3> p, uint32_t mat)
        {
          auto a = p[0], b = p[1], d = p[2];
          double nx = (b.y - a.y) * (d.z - a.z) - (b.z - a.z) * (d.y - a.y), ny = (b.z - a.z) * (d.x - a.x) - (b.x - a.x) * (d.z - a.z), nz = (b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x),
                 length = std::sqrt(nx * nx + ny * ny + nz * nz);
          // Cull only triangles actually lost or reversed by the renderer's float
          // conversion. A fixed double-area cutoff discards valid thin faces.
          float ux = float(b.x) - float(a.x), uy = float(b.y) - float(a.y), uz = float(b.z) - float(a.z);
          float vx = float(d.x) - float(a.x), vy = float(d.y) - float(a.y), vz = float(d.z) - float(a.z);
          float fx = uy * vz - uz * vy, fy = uz * vx - ux * vz, fz = ux * vy - uy * vx;
          double packedLength = std::sqrt(double(fx) * fx + double(fy) * fy + double(fz) * fz);
          if (length == 0 || packedLength <= 1e-8 || fx * nx + fy * ny + fz * nz <= 0)
          {
            ++omitted;
            omittedArea += length * .5;
            return;
          }
          int first = packed["mesh"]["vertices"].size();
          for (auto v : p)
            packed["mesh"]["vertices"].push_back({v.x, v.y, v.z});
          packed["mesh"]["triangles"].push_back({first, first + 1, first + 2});
          packed["mesh"]["materials"].push_back(mat);
          packed["mesh"]["expectedNormals"].push_back({nx / length, ny / length, nz / length});
        };
        for (auto &t : mesh.triangles)
          emit({mesh.vertices[t.vertices[0]], mesh.vertices[t.vertices[1]], mesh.vertices[t.vertices[2]]}, t.material);
        for (auto &t : instances)
          emit(t.points, t.material);
        packed["renderOmittedTinyTriangles"] = omitted;
        packed["renderOmittedAreaMetres2"] = omittedArea;
        check(omittedArea < .0001, tag + " float render packing omission below 1 square centimetre");
        std::ofstream(output / (tag + "-mesh.json")) << packed.dump();
      }
      if (preset == 1 || preset == 3)
      {
        auto parcelSettings = s;
        parcelSettings.blockHousing = true;
        parcelSettings.frontageGap = .3;
        if (recipe == 0)
        {
          parcelSettings.recipe = CityRecipe::PlannedBlocks;
          parcelSettings.targetBlockArea = 4000;
          parcelSettings.plannedAxisRadians = .12;
        }
        CityLayout parcels;
        bool parcelOK = generateCityLayout(domain, reservations, arteries, gates, parcelSettings, terrain, parcels, &error);
        check(parcelOK && !parcels.objects.empty(), tag + " physical block parcels handle holes/concave coast: " + error);
        if (parcelOK)
        {
          check(meshCityInstances(parcels, terrain, instances, &error), tag + " polygonal parcels mesh on prepared land: " + error);
          if (!output.empty())
          {
            auto j = encode(parcels);
            j["envelope"] = polygon(domain.outer);
            j["holes"] = json::array();
            j["reservations"] = json::array();
            j["walls"] = json::array();
            for (const auto &p : domain.holes)
              j["holes"].push_back(polygon(p));
            for (const auto &p : reservations)
              j["reservations"].push_back(polygon(p));
            std::ofstream(output / (tag + "-blocks.json")) << j.dump(2);
          }
        }
      }
      std::cout << "CITY_METRIC " << tag << " wards=" << c.wards.size() << " streets=" << c.streets.size() << " occupied=" << occupied << " gaps=" << c.rejectedAreas << " faces=" << map.faces.size()
                << " triangles=" << mesh.triangles.size() << '\n';
    }
  // A city-scale visual fixture exercises the existing generator with a market
  // reservation and three arrivals; the small fixtures above remain regressions.
  {
    CityPolygon town{{{0, 0, 65}, {60, 0, 10}, {210, 0, 0}, {290, 0, 40}, {320, 0, 100}, {300, 0, 210}, {225, 0, 260}, {100, 0, 250}, {0, 0, 180}}, {}};
    CitySettings settings;
    settings.seed = 719;
    settings.siteSpacing = 38;
    settings.maximumSites = 48;
    settings.targetBlockArea = 2200;
    settings.streetHalfWidth = 1.8;
    settings.frontageGap = .85;
    settings.setback = .7;
    settings.walls = true;
    std::vector<CityStreet> streets{
      {1, {0, 0, 85}, {160, 0, 105}, 2.75, true}, {2, {160, 0, 105}, {320, 0, 100}, 2.75, true}, {3, {190, 0, 104.0625}, {225, 0, 260}, 2.5, true}, {4, {160, 0, 105}, {160, 0, 112}, 1.25, true}};
    std::vector<CityGate> gates{{1, {0, 0, 85}, 1}, {2, {320, 0, 100}, 2}, {3, {225, 0, 260}, 3}};
    std::vector<Poly> reservations{rect(140, 112, 35, 32)};
    auto flat = [](double, double)
    {
      return 0.;
    };
    CityLayout townLayout;
    std::string error;
    const bool ok = generateCityLayout(town, reservations, streets, gates, settings, flat, townLayout, &error);
    check(ok, "city-scale mixed frontage: " + error);
    if (ok)
    {
      std::set<CityBuildingType> types;
      size_t compounds = 0, buildings = 0;
      bool wingsFit = true;
      for (const auto &o : townLayout.objects)
      {
        if (o.kind == CityObjectKind::RejectedGap || o.kind == CityObjectKind::Garden)
          continue;
        types.insert(o.buildingType);
        ++buildings;
        compounds += o.parts.size() > 1;
        for (const auto &part : o.parts)
          wingsFit &= part.footprint.size() == 4 && part.height > 0;
      }
      check(types.size() == 6 && compounds >= 15 && buildings >= 100, "city has six building programs and varied compound massing");
      check(townLayout.wards.size() < townLayout.subdivisionCells, "discarded streets dissolve obsolete allocation boundaries");
      std::vector<CityInstanceTriangle> triangles;
      check(wingsFit && meshCityInstances(townLayout, flat, triangles, &error) && !triangles.empty(), "city compound wings mesh from authoritative geometry");
      CityLayout repeated;
      check(generateCityLayout(town, reservations, streets, gates, settings, flat, repeated, &error) && encode(repeated) == encode(townLayout), "city programs and wing geometry are seed deterministic");
      auto invalid = townLayout;
      auto compound = std::find_if(
          invalid.objects.begin(),
          invalid.objects.end(),
          [](const auto &o)
          {
            return o.parts.size() > 1;
          });
      const auto triangleCount = triangles.size();
      if (compound != invalid.objects.end())
        compound->parts.push_back(compound->parts.front());
      check(!meshCityInstances(invalid, flat, triangles, &error) && triangles.size() == triangleCount, "overlapping wings fail meshing atomically");
      SurfaceMapSettings mapSettings;
      mapSettings.maxX = 320;
      mapSettings.maxZ = 260;
      mapSettings.maximumFaces = 20000;
      SurfaceArrangement arranged;
      SurfaceArrangementMesh surfaceMesh;
      check(
          arrangeSurfaceContours(townLayout.contours, mapSettings, flat, 1, arranged, &error) && meshSurfaceArrangement(arranged, flat, {}, surfaceMesh, &error) && std::abs(surfaceMesh.groundArea - 83200) < 1e-4,
          "city-scale streets preserve authoritative surface coverage: " + error);
      if (!output.empty())
      {
        auto j = encode(townLayout);
        j["envelope"] = polygon(town.outer);
        j["holes"] = json::array();
        j["reservations"] = json::array({polygon(reservations[0])});
        j["walls"] = json::array();
        for (const auto &wall : townLayout.walls)
          j["walls"].push_back({polygon({wall.a, wall.b}), wall.halfWidth});
        std::ofstream(output / "market-city.json") << j.dump(2);
      }
      settings.blockHousing = true;
      settings.regularBuildingShells = false;
      settings.targetPlotArea = 110;
      settings.frontageGap = .3;

      // Test policy masks model ordinary districts. Landmarks remain explicit
      // reservations; these masks do not invent castle or harbor eligibility.
      settings.housingAreas = {{10, DistrictUse::Suburb, {rect(0, 0, 320, 90)}, 65}, {20, DistrictUse::Merchant, {rect(0, 90, 200, 170)}, 150}, {30, DistrictUse::Manor, {rect(200, 90, 120, 170)}, 300}};
      CityLayout blockLayout;
      error.clear();
      bool blockOK = generateCityLayout(town, reservations, streets, gates, settings, flat, blockLayout, &error);
      check(blockOK, "large city uses final street blocks and parcel allocation: " + error);
      if (blockOK)
      {
        check(blockLayout.objects.size() > buildings, "block subdivision packs more housing than detached frontage on the same streets");
        check(meshCityInstances(blockLayout, flat, triangles, &error), "irregular city parcels produce actual roof meshes: " + error);
        std::set<uint64_t> districts;
        size_t courts = 0, irregular = 0;
        for (const auto &b : blockLayout.blocks)
        {
          districts.insert(b.district);
          courts += b.courtyards.size();
        }
        for (const auto &o : blockLayout.objects)
          irregular += o.parts.front().footprint.size() != 4;
        check(districts.size() >= 3 && courts > 0 && irregular > 0, "district policies retain inner courts and polygonal corner plots");
        check(encode(blockLayout)["streets"] == encode(townLayout)["streets"], "housing policies preserve the small-town reference street network");
        std::map<uint64_t, std::pair<double, size_t>> plotStats;
        for (const auto &o : blockLayout.objects)
        {
          double area = 0;
          for (size_t k = 0; k < o.footprint.size(); ++k)
          {
            auto a = o.footprint[k], b = o.footprint[(k + 1) % o.footprint.size()];
            area += (a.x * b.z - a.z * b.x) * .5;
          }
          auto &stats = plotStats[blockLayout.blocks[o.block - 1].district];
          stats.first += area;
          ++stats.second;
        }
        auto mean = [&](uint64_t id)
        {
          return plotStats[id].first / std::max(size_t(1), plotStats[id].second);
        };
        check(
            plotStats[10].second > 40 && plotStats[20].second > 10 && plotStats[30].second > 10 && mean(20) > mean(10) * 1.6 && mean(30) > mean(10) * 3,
            "packed housing and larger merchant/manor plots have distinct measured sizes");
        CityLayout repeatedBlocks;
        std::reverse(settings.housingAreas.begin(), settings.housingAreas.end());
        check(generateCityLayout(town, reservations, streets, gates, settings, flat, repeatedBlocks, &error) && encode(repeatedBlocks) == encode(blockLayout), "block plots deterministic under housing policy order");
        if (!output.empty())
        {
          auto j = encode(blockLayout);
          j["envelope"] = polygon(town.outer);
          j["holes"] = json::array();
          j["reservations"] = json::array({polygon(reservations[0])});
          j["walls"] = json::array();
          for (const auto &wall : blockLayout.walls)
            j["walls"].push_back({polygon({wall.a, wall.b}), wall.halfWidth});
          std::ofstream(output / "block-city.json") << j.dump(2);
          json packed;
          packed["phase"] = 11;
          for (const auto *key : {"vertices", "triangles", "materials", "expectedNormals"})
            packed["mesh"][key] = json::array();
          double omittedArea = 0;
          auto emit = [&](std::array<Position, 3> p, uint32_t material)
          {
            auto a = p[0], b = p[1], c = p[2];
            double nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y), ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z), nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            double length = std::sqrt(nx * nx + ny * ny + nz * nz);
            float ux = float(b.x) - float(a.x), uy = float(b.y) - float(a.y), uz = float(b.z) - float(a.z), vx = float(c.x) - float(a.x), vy = float(c.y) - float(a.y), vz = float(c.z) - float(a.z);
            float fx = uy * vz - uz * vy, fy = uz * vx - ux * vz, fz = ux * vy - uy * vx;
            if (length == 0 || std::sqrt(double(fx) * fx + double(fy) * fy + double(fz) * fz) <= 1e-8 || double(fx) * nx + double(fy) * ny + double(fz) * nz <= 0)
            {
              omittedArea += length * .5;
              return;
            }
            size_t first = packed["mesh"]["vertices"].size();
            for (auto q : p)
              packed["mesh"]["vertices"].push_back({q.x, q.y, q.z});
            packed["mesh"]["triangles"].push_back({first, first + 1, first + 2});
            packed["mesh"]["materials"].push_back(material);
            packed["mesh"]["expectedNormals"].push_back({nx / length, ny / length, nz / length});
          };
          for (const auto &t : surfaceMesh.triangles)
            emit({surfaceMesh.vertices[t.vertices[0]], surfaceMesh.vertices[t.vertices[1]], surfaceMesh.vertices[t.vertices[2]]}, t.material);
          for (const auto &t : triangles)
            emit(t.points, t.material);
          check(omittedArea < .0005, "city-scale polygonal block float omission below five square centimetres");
          packed["renderOmittedAreaMetres2"] = omittedArea;
          std::ofstream(output / "block-city-mesh.json") << packed.dump();
        }
        std::cout << "BLOCK_CITY_METRIC blocks=" << blockLayout.blocks.size() << " buildings=" << blockLayout.objects.size() << " courtyardPieces=" << courts << " irregular=" << irregular << '\n';
      }
      for (int variant = 0; variant < 3; ++variant)
      {
        auto plannedSettings = settings;
        plannedSettings.recipe = CityRecipe::PlannedBlocks;
        plannedSettings.seed = 719 + variant;
        plannedSettings.targetBlockArea = variant == 0 ? 6000 : variant == 1 ? 8500 : 7000;
        plannedSettings.streetHalfWidth = 2.2;
        plannedSettings.plannedAxisRadians = variant == 0 ? 0 : variant == 1 ? .12 : -.16;
        plannedSettings.plannedDistrictTurnRadians = variant == 1 ? .2 : 0;
        plannedSettings.plannedBlockAspect = variant == 2 ? 2.4 : 1.5;
        std::vector<Poly> plannedReservations = reservations;
        if (variant == 1)
          plannedReservations.push_back(rect(220, 160, 50, 55));
        CityLayout plannedCity;
        error.clear();
        bool plannedOK = generateCityLayout(town, plannedReservations, streets, gates, plannedSettings, flat, plannedCity, &error);
        check(plannedOK, "planned city alternative " + std::to_string(variant) + ": " + error);
        if (plannedOK)
        {
          check(
              plannedCity.sites.empty() && plannedCity.subdivisionCells < townLayout.subdivisionCells / 2 && plannedCity.streets.size() < townLayout.streets.size(),
              "planned city uses fewer coarse cells and street segments " + std::to_string(variant));
          std::set<uint8_t> hierarchy;
          for (const auto &road : plannedCity.streets)
            if (!road.arterial)
              hierarchy.insert(road.hierarchy);
          check(hierarchy.size() >= 2 && plannedCity.objects.size() > 80, "planned avenues and ordinary streets support populated blocks " + std::to_string(variant));
          if (variant == 0)
          {
            auto repeatedSettings = plannedSettings;
            repeatedSettings.maximumSites = 4;
            CityLayout repeatedPlan;
            check(
                generateCityLayout(town, plannedReservations, streets, gates, repeatedSettings, flat, repeatedPlan, &error) && encode(repeatedPlan) == encode(plannedCity),
                "planned streets and parcels are deterministic and independent of Poisson cell budget");
          }
          check(meshCityInstances(plannedCity, flat, triangles, &error), "planned city polygonal buildings mesh " + std::to_string(variant) + ": " + error);
          if (!output.empty())
          {
            auto j = encode(plannedCity);
            j["envelope"] = polygon(town.outer);
            j["holes"] = json::array();
            j["reservations"] = json::array();
            j["walls"] = json::array();
            for (const auto &p : plannedReservations)
              j["reservations"].push_back(polygon(p));
            for (const auto &wall : plannedCity.walls)
              j["walls"].push_back({polygon({wall.a, wall.b}), wall.halfWidth});
            j["recipe"] = "planned";
            j["variant"] = variant;
            std::ofstream(output / ("planned-city-" + std::to_string(variant) + ".json")) << j.dump(2);
          }
          std::cout << "PLANNED_CITY_METRIC variant=" << variant << " cells=" << plannedCity.subdivisionCells << " blocks=" << plannedCity.blocks.size() << " streets=" << plannedCity.streets.size()
                    << " buildings=" << plannedCity.objects.size() << '\n';
        }
      }
      {
        auto capitalSettings = settings;
        capitalSettings.regularBuildingShells = true;
        capitalSettings.recipe = CityRecipe::PlannedBlocks;
        capitalSettings.seed = 722;
        capitalSettings.targetBlockArea = 2600;
        capitalSettings.plannedAxisRadians = .12;
        capitalSettings.plannedDistrictTurnRadians = .08;
        capitalSettings.plannedBlockAspect = 1.6;
        capitalSettings.streetHalfWidth = 2.2;
        capitalSettings.minimumWidth = 4;
        capitalSettings.minimumDepth = 5;
        capitalSettings.targetPlotArea = 65;
        capitalSettings.housingAreas.clear();
        capitalSettings.emitBuildings = false;
        CityLayout planningLayout;
        error.clear();
        bool planningOK = generateCityLayout(town, reservations, streets, gates, capitalSettings, flat, planningLayout, &error);
        check(planningOK, "capital planning pass keeps streets without placing houses: " + error);
        if (planningOK)
        {
          std::vector<CityPlanCell> cells;
          auto inside = [](const Poly &p, Position q)
          {
            for (size_t k = 0; k < p.size(); ++k)
            {
              auto a = p[k], b = p[(k + 1) % p.size()];
              if ((b.x - a.x) * (q.z - a.z) - (b.z - a.z) * (q.x - a.x) < -1e-6)
                return false;
            }
            return true;
          };
          auto corridor = [](const CityStreet &s)
          {
            double len = std::hypot(s.b.x - s.a.x, s.b.z - s.a.z), nx = -(s.b.z - s.a.z) / len * s.halfWidth, nz = (s.b.x - s.a.x) / len * s.halfWidth;
            return Poly{{s.a.x - nx, 0, s.a.z - nz}, {s.b.x - nx, 0, s.b.z - nz}, {s.b.x + nx, 0, s.b.z + nz}, {s.a.x + nx, 0, s.a.z + nz}};
          };
          for (const auto &ward : planningLayout.wards)
            for (const auto &p : ward.pieces)
            {
              CityPlanCell cell;
              cell.id = cells.size() + 1;
              cell.boundary = p;
              Position center{};
              for (auto q : p)
              {
                center.x += q.x / p.size();
                center.z += q.z / p.size();
              }
              // Synthetic suitability fixture, not inferred world elevation,
              // security, fertility or shoreline. The world adapter must supply it.
              cell.defense = center.z > 150 ? .9 : .3;
              cell.secure = center.z > 120;
              cell.fertility = center.x > 200 ? .8 : .4;
              for (const auto &road : streets)
                if (cityPolygonsOverlap(p, corridor(road)))
                  cell.allocatable = false;
              double area = 0;
              for (size_t k = 0; k < p.size(); ++k)
              {
                auto a = p[k], b = p[(k + 1) % p.size()];
                area += (a.x * b.z - a.z * b.x) * .5;
              }
              if (area < 900)
                cell.allocatable = false;
              cells.push_back(cell);
            }
          std::vector<uint64_t> gateCells;
          for (const auto &gate : gates)
            for (const auto &cell : cells)
              if (inside(cell.boundary, gate.position))
              {
                gateCells.push_back(cell.id);
                break;
              }
          std::vector<DistrictRequest> requests{
            {1, DistrictUse::Citadel, 600, 1800, true},
            {2, DistrictUse::Castle, 500, 1300, true},
            {3, DistrictUse::Manor, 400, 1200, true},
            {4, DistrictUse::Merchant, 600, 2500, true},
            {5, DistrictUse::Guild, 300, 900, true},
            {6, DistrictUse::Military, 500, 1400, true},
            {7, DistrictUse::Temple, 400, 1100, true},
            {8, DistrictUse::Church, 250, 650, true},
            {9, DistrictUse::Civic, 300, 850, true},
            {10, DistrictUse::Residential, 600, 7000, true},
            {11, DistrictUse::Craftsmen, 600, 1800, true}};
          // Targets are gross precinct areas (including courts/access), not
          // the eventual temple, guildhall or keep footprint.
          for (auto &request : requests)
            request.targetArea = std::max(request.targetArea, 3000.);
          CityPlanSettings planSettings;
          planSettings.seed = capitalSettings.seed;
          CityMasterPlan plan;
          bool planOK = generateCityMasterPlan(cells, requests, gateCells, planSettings, plan, &error);
          check(planOK, "capital allocates required civic, military, religious and commercial uses: " + error);
          if (planOK)
          {
            bool allocationsValid = true;
            for (const auto &district : plan.districts)
              for (auto id : district.cells)
              {
                const auto &cell = cells[id - 1];
                allocationsValid &= cell.allocatable;
                if (district.use == DistrictUse::Citadel)
                  allocationsValid &= cell.secure && cell.defense >= .6;
                if (district.use == DistrictUse::Castle)
                  allocationsValid &= cell.defense >= .6;
                if (district.use == DistrictUse::Military)
                  allocationsValid &= cell.secure;
              }
            check(allocationsValid && plan.districts.size() == 11, "districts preserve circulation and hard defensive eligibility");
            capitalSettings.emitBuildings = true;
            capitalSettings.districts = plan.districts;
            CityLayout capital;
            bool capitalOK = generateCityLayout(town, reservations, streets, gates, capitalSettings, flat, capital, &error);
            check(capitalOK, "capital reserves compounds before fine housing subdivision: " + error);
            if (capitalOK)
            {
              size_t alleys = 0;
              for (const auto &road : capital.streets)
                alleys += !road.arterial && road.halfWidth < 1.5;
              check(capital.objects.size() > 250 && alleys > 0 && capital.subdivisionCells > 20, "smaller housing blocks and narrow alleys preserve the planned structure");
              check(meshCityInstances(capital, flat, triangles, &error), "capital housing massing remains valid: " + error);
              SurfaceMapSettings capitalBounds;
              capitalBounds.maxX = 320;
              capitalBounds.maxZ = 260;
              capitalBounds.maximumFaces = 20000;
              SurfaceArrangement capitalMap;
              SurfaceArrangementMesh capitalMesh;
              check(
                  arrangeSurfaceContours(capital.contours, capitalBounds, flat, 1, capitalMap, &error) && meshSurfaceArrangement(capitalMap, flat, {}, capitalMesh, &error) &&
                      std::abs(capitalMesh.groundArea - 83200) < 1e-4,
                  "capital district exclusions and alleys preserve full surface coverage: " + error);

              if (!output.empty())
              {
                auto j = encode(capital);
                j["envelope"] = polygon(town.outer);
                j["holes"] = json::array();
                j["reservations"] = json::array();
                j["walls"] = json::array();
                for (const auto &p : capital.reservations)
                  j["reservations"].push_back(polygon(p));
                for (const auto &wall : capital.walls)
                  j["walls"].push_back({polygon({wall.a, wall.b}), wall.halfWidth});
                j["suitabilitySource"] = "synthetic fixture predicates; world integration pending";
                std::ofstream(output / "planned-capital.json") << j.dump(2);
              }
              auto outskirtsSettings = capitalSettings;
              outskirtsSettings.wallSetback = 8;
              // Synthetic dry land around the fixture. A world adapter must
              // supply terrain/water/ownership-filtered masks here.
              outskirtsSettings.suburbLand = {rect(-80, -80, 480, 420)};
              CityLayout outskirts;
              bool outskirtsOK = generateCityLayout(town, reservations, streets, gates, outskirtsSettings, flat, outskirts, &error);
              check(outskirtsOK, "capital grows gate suburbs on supplied buildable land: " + error);
              if (outskirtsOK)
              {
                size_t exteriorHouses = 0;
                bool clearance = true, originalStreets = true;
                for (const auto &object : outskirts.objects)
                {
                  exteriorHouses += object.outsideWalls;
                  for (const auto &part : object.parts)
                    for (auto p : part.footprint)
                      for (const auto &wall : outskirts.walls)
                      {
                        double dx = wall.b.x - wall.a.x, dz = wall.b.z - wall.a.z;
                        double t = std::clamp(((p.x - wall.a.x) * dx + (p.z - wall.a.z) * dz) / (dx * dx + dz * dz), 0., 1.);
                        clearance &= std::hypot(p.x - wall.a.x - t * dx, p.z - wall.a.z - t * dz) >= wall.halfWidth + outskirtsSettings.wallSetback - 1e-5;
                      }
                }
                auto original = encode(capital)["streets"], expanded = encode(outskirts)["streets"];
                for (size_t k = 0; k < original.size(); ++k)
                  originalStreets &= original[k] == expanded[k];
                check(exteriorHouses >= 12 && outskirts.suburbs.size() >= 3, "multiple gates grow road-fronting exterior cottages");
                check(clearance, "all capital and suburb roofs keep eight metres clear of wall faces and ends");
                check(originalStreets && outskirts.streets.size() > capital.streets.size() + gates.size(), "outer neighbourhood lanes join approaches without changing city streets");
                CityLayout repeat;
                auto reversedGates = gates;
                std::reverse(reversedGates.begin(), reversedGates.end());
                check(
                    generateCityLayout(town, reservations, streets, reversedGates, outskirtsSettings, flat, repeat, &error) && encode(repeat) == encode(outskirts),
                    "gate suburb geometry is deterministic under gate input order");
                check(meshCityInstances(outskirts, flat, triangles, &error), "suburb housing and wall meshes remain valid: " + error);
                capitalBounds.minX = -80;
                capitalBounds.minZ = -80;
                capitalBounds.maxX = 400;
                capitalBounds.maxZ = 340;
                check(
                    arrangeSurfaceContours(outskirts.contours, capitalBounds, flat, 1, capitalMap, &error) && meshSurfaceArrangement(capitalMap, flat, {}, capitalMesh, &error) &&
                        std::abs(capitalMesh.groundArea - 480 * 420) < 1e-4,
                    "city and suburbs preserve full map coverage: " + error);
                if (!output.empty())
                {
                  auto j = encode(outskirts);
                  j["envelope"] = polygon(town.outer);
                  j["holes"] = json::array();
                  j["reservations"] = json::array();
                  j["walls"] = json::array();
                  j["suburbLand"] = json::array();
                  for (const auto &p : outskirtsSettings.suburbLand)
                    j["suburbLand"].push_back(polygon(p));
                  for (const auto &p : outskirts.reservations)
                    j["reservations"].push_back(polygon(p));
                  for (const auto &wall : outskirts.walls)
                    j["walls"].push_back({polygon({wall.a, wall.b}), wall.halfWidth});
                  j["suitabilitySource"] = "synthetic dry suburb masks; world integration pending";
                  std::ofstream(output / "capital-suburbs.json") << j.dump(2);
                }
                auto largeSettings = outskirtsSettings;
                largeSettings.wallCornerInset = 26;
                largeSettings.wallSectionLength = 40;
                largeSettings.targetBlockArea *= 1.8;
                largeSettings.suburbRoadLength = 100;
                largeSettings.maximumSuburbWards = 32;
                largeSettings.suburbGrowthRadius = 85;
                largeSettings.suburbCellSpacing = 38;
                largeSettings.suburbLand = {rect(-140, -100, 800, 650)};
                auto largeTown = town;
                auto largeStreets = streets;
                auto largeGates = gates;
                auto largeReservations = reservations;
                auto scale = [](Poly &p)
                {
                  for (auto &v : p)
                  {
                    v.x *= 1.6;
                    v.z *= 1.6;
                  }
                };
                scale(largeTown.outer);
                for (auto &road : largeStreets)
                {
                  road.a.x *= 1.6;
                  road.a.z *= 1.6;
                  road.b.x *= 1.6;
                  road.b.z *= 1.6;
                }
                for (auto &gate : largeGates)
                {
                  gate.position.x *= 1.6;
                  gate.position.z *= 1.6;
                }
                for (auto &p : largeReservations)
                  scale(p);
                for (auto &district : largeSettings.districts)
                {
                  for (auto &p : district.pieces)
                    scale(p);
                  scale(district.access);
                  district.area *= 2.56;
                }
                CityLayout large;
                bool largeOK = generateCityLayout(largeTown, largeReservations, largeStreets, largeGates, largeSettings, flat, large, &error);
                check(largeOK, "larger capital with faceted walls and grown outer wards: " + error);
                if (largeOK)
                {
                  size_t external = 0, compounds = 0, lateralHouses = 0;
                  std::set<CityBuildingType> exteriorTypes;
                  bool orthogonal = true, wallResolution = true;
                  for (const auto &wall : large.walls)
                    wallResolution &= std::hypot(wall.a.x - wall.b.x, wall.a.z - wall.b.z) <= largeSettings.wallSectionLength + 1e-6;
                  for (const auto &object : large.objects)
                  {
                    external += object.outsideWalls;
                    compounds += object.parts.size() > 1;
                    if (object.outsideWalls)
                    {
                      exteriorTypes.insert(object.buildingType);
                      double nearestGate = 1e30, nearestWall = 1e30;
                      for (const auto &gate : largeGates)
                        nearestGate = std::min(nearestGate, std::hypot(object.entrance.x - gate.position.x, object.entrance.z - gate.position.z));
                      for (const auto &wall : large.walls)
                      {
                        double dx = wall.b.x - wall.a.x, dz = wall.b.z - wall.a.z;
                        double t = std::clamp(((object.entrance.x - wall.a.x) * dx + (object.entrance.z - wall.a.z) * dz) / (dx * dx + dz * dz), 0., 1.);
                        nearestWall = std::min(nearestWall, std::hypot(object.entrance.x - wall.a.x - t * dx, object.entrance.z - wall.a.z - t * dz));
                      }
                      lateralHouses += nearestGate > largeSettings.suburbCellSpacing * 1.8 && nearestWall < largeSettings.suburbCellSpacing;
                    }
                    for (const auto &part : object.parts)
                    {
                      orthogonal &= part.footprint.size() == 4;
                      for (size_t i = 0; i < part.footprint.size(); ++i)
                      {
                        auto a = part.footprint[i], b = part.footprint[(i + 1) % 4], c = part.footprint[(i + 2) % 4];
                        orthogonal &= std::abs((b.x - a.x) * (c.x - b.x) + (b.z - a.z) * (c.z - b.z)) < 1e-5;
                      }
                    }
                  }
                  check(orthogonal && compounds > 0, "regular house shells and L/U compounds replace acute polygon roofs");
                  check(external > 50 && exteriorTypes.size() >= 3, "outer neighbourhoods contain varied housing and workshop forms");
                  check(lateralHouses > 0, "suburbs develop wall frontage away from the gate approaches");
                  check(wallResolution && large.envelope.outer.size() > largeTown.outer.size(), "faceted wall envelope has shorter segments and pinned gates");
                  bool joined = !large.towers.empty();
                  for (const auto &wall : large.walls)
                    for (auto end : {wall.a, wall.b})
                      joined &= std::any_of(
                          large.towers.begin(),
                          large.towers.end(),
                          [&](const auto &t)
                          {
                            bool contains = true;
                            for (size_t k = 0; k < t.footprint.size(); ++k)
                            {
                              auto a = t.footprint[k], b = t.footprint[(k + 1) % t.footprint.size()];
                              contains &= (b.x - a.x) * (end.z - a.z) - (b.z - a.z) * (end.x - a.x) >= -1e-6;
                            }
                            return contains;
                          });
                  check(joined, "every straight wall section joins towers at both ends");
                  check(meshCityInstances(large, flat, triangles, &error), "large capital instances mesh: " + error);
                  capitalBounds.minX = -140;
                  capitalBounds.minZ = -100;
                  capitalBounds.maxX = 660;
                  capitalBounds.maxZ = 550;
                  check(
                      arrangeSurfaceContours(large.contours, capitalBounds, flat, 1, capitalMap, &error) && meshSurfaceArrangement(capitalMap, flat, {}, capitalMesh, &error),
                      "large capital surface coverage meshes: " + error);
                  if (!output.empty())
                  {
                    auto j = encode(large);
                    j["envelope"] = polygon(large.envelope.outer);
                    j["holes"] = json::array();
                    j["reservations"] = json::array();
                    j["walls"] = json::array();
                    j["suburbLand"] = json::array();
                    for (const auto &p : large.reservations)
                      j["reservations"].push_back(polygon(p));
                    for (const auto &wall : large.walls)
                      j["walls"].push_back({polygon({wall.a, wall.b}), wall.halfWidth});
                    for (const auto &p : largeSettings.suburbLand)
                      j["suburbLand"].push_back(polygon(p));
                    j["regularBuildingShells"] = true;
                    j["suitabilitySource"] = "synthetic flat land and scaled district masks; population planner pending";
                    std::ofstream(output / "large-capital.json") << j.dump(2);
                  }
                  std::cout << "LARGE_METRIC buildings=" << large.objects.size() << " exterior=" << external << " compounds=" << compounds << " walls=" << large.walls.size() << " streets=" << large.streets.size()
                            << '\n';
                }
                std::cout << "SUBURB_METRIC buildings=" << outskirts.objects.size() << " outside=" << exteriorHouses << " wallSetback=" << outskirtsSettings.wallSetback << '\n';
              }
              std::cout << "CAPITAL_METRIC cells=" << capital.subdivisionCells << " blocks=" << capital.blocks.size() << " streets=" << capital.streets.size() << " alleys=" << alleys
                        << " houses=" << capital.objects.size() << " districts=" << plan.districts.size() << '\n';
            }
          }
        }
      }
      std::cout << "CITY_PROGRAM_METRIC buildings=" << buildings << " compounds=" << compounds << " types=" << types.size() << '\n';
    }
  }
  CityPolygon d{rect(0, 0, 140, 120), {}};
  CitySettings s;
  CityLayout c;
  c.revision = 999;
  std::string e;
  std::vector<CityStreet> a{{1, {0, 0, 28}, {140, 0, 28}, 3, true}};
  std::vector<CityGate> g{{1, {0, 0, 28}, 1}};
  auto flat = [](double, double)
  {
    return 0.;
  };
  {
    auto suburban = s;
    suburban.recipe = CityRecipe::PlannedBlocks;
    suburban.blockHousing = true;
    suburban.walls = true;
    suburban.suburbLand = {rect(-65, 0, 65, 120), rect(-90, 0, 10, 120)};
    auto excludes = std::vector<Poly>{rect(-39, 20, 3, 16)};
    CityLayout clipped;
    check(generateCityLayout(d, excludes, a, g, suburban, flat, clipped, &e), "suburb approach stops at an exterior reservation: " + e);
    bool stops = !clipped.suburbs.empty();
    for (const auto &street : clipped.streets)
      if (std::hypot(street.a.x - g[0].position.x, street.a.z - g[0].position.z) < 1e-6 && street.b.x < 0)
        stops &= street.b.x >= -36.00001;
    for (const auto &piece : clipped.suburbs)
      for (auto v : piece)
        stops &= v.x >= -65.00001;
    check(stops, "suburb road never jumps excluded land to a farther buildable island");
    CityLayout empty;
    suburban.suburbLand = {rect(-90, 0, 20, 120)};
    check(generateCityLayout(d, {}, a, g, suburban, flat, empty, &e) && empty.suburbs.empty(), "disconnected buildable countryside produces no suburb");
    suburban.suburbLand = {rect(-65, 0, 65, 120)};
    auto slope = [](double x, double)
    {
      return x < 0 ? x * .1 : 0.;
    };
    check(!generateCityLayout(d, {}, a, g, suburban, slope, c, &e) && c.revision == 999, "unprepared exterior road terrain fails atomically");
    suburban.wallSetback = -1;
    check(!generateCityLayout(d, {}, a, g, suburban, flat, c, &e) && c.revision == 999, "negative wall setback fails atomically");
  }
  auto invalidHousing = s;
  invalidHousing.blockHousing = true;
  invalidHousing.housingAreas = {{1, DistrictUse::Residential, {rect(0, 0, 40, 40)}, 100}, {2, DistrictUse::Manor, {rect(20, 20, 40, 40)}, 300}};
  check(!generateCityLayout(d, {}, a, g, invalidHousing, flat, c, &e) && c.revision == 999, "overlapping housing policies fail atomically");
  invalidHousing.housingAreas.resize(1);
  invalidHousing.housingAreas[0].use = DistrictUse::Castle;
  check(!generateCityLayout(d, {}, a, g, invalidHousing, flat, c, &e) && c.revision == 999, "castle requires a compound reservation, not an ordinary housing mask");
  s.maximumObjects = 1;
  check(!generateCityLayout(d, {}, a, g, s, flat, c, &e) && c.revision == 999, "object budget fails atomically");
  s = CitySettings{};
  g[0].externalRoad = 5;
  check(!generateCityLayout(d, {}, a, g, s, flat, c, &e), "unnamed disconnected gate rejected");
  g[0].externalRoad = 1;
  s.minimumWidth = 0;
  check(!generateCityLayout(d, {}, a, g, s, flat, c, &e), "needle dimensions rejected");
  s = CitySettings{};
  s.recipe = CityRecipe::RecursiveBlocks;
  s.maximumSplitDepth = 0;
  check(generateCityLayout(d, {}, a, g, s, flat, c, &e) && c.splitAttempts == 0, "bounded forced unsplit block terminates");
  s = CitySettings{};
  s.maximumSites = 4;
  s.targetBlockArea = 1200;
  bool refined = generateCityLayout(d, {}, a, g, s, flat, c, &e);
  double largest = 0, totalArea = 0;
  for (const auto &ward : c.wards)
  {
    double wardArea = 0;
    for (const auto &piece : ward.pieces)
      for (size_t k = 0; k < piece.size(); ++k)
        wardArea += (piece[k].x * piece[(k + 1) % piece.size()].z - piece[k].z * piece[(k + 1) % piece.size()].x) * .5;
    largest = std::max(largest, wardArea);
    totalArea += wardArea;
  }
  check(refined && c.splitAttempts > 0 && c.subdivisionCells > c.sites.size() && largest < 16800 && std::abs(totalArea - 16800) < 1e-5, "coarse Voronoi patches refine without losing land or forcing thin blocks");
  s.maximumSplitDepth = 0;
  check(generateCityLayout(d, {}, a, g, s, flat, c, &e) && c.splitAttempts == 0 && c.subdivisionCells == c.sites.size(), "Voronoi refinement respects explicit depth budget");
  s = CitySettings{};
  s.recipe = CityRecipe::RecursiveBlocks;
  s.minimumWidth = s.maximumWidth = 45;
  s.minimumDepth = s.maximumDepth = 24;
  check(generateCityLayout(d, {}, a, g, s, flat, c, &e) && c.wards.size() == 1 && c.unrefinedBlocks == 1, "retain oversized block when requested frontage depth cannot survive a cut");
  s = CitySettings{};
  check(!generateCityLayout(d, {rect(60, 20, 20, 20)}, a, g, s, flat, c, &e), "fixed artery cannot cross protected water");
  check(!generateCityLayout(d, {rect(60, 27.99, .01, .02)}, a, g, s, flat, c, &e), "sub-sample-width water crossing rejected exactly");
  auto badA = a;
  badA[0].a.x = std::numeric_limits<double>::quiet_NaN();
  check(!generateCityLayout(d, {}, badA, g, s, flat, c, &e), "nonfinite arterial rejected before clipping");
  auto interiorGate = g;
  interiorGate[0].position = {20, 0, 28};
  check(!generateCityLayout(d, {}, a, interiorGate, s, flat, c, &e), "gate must touch envelope");
  auto steep = [](double x, double z)
  {
    return 10 * x + 10 * z;
  };
  check(!generateCityLayout(d, {}, a, g, s, steep, c, &e), "unprepared hillside cannot silently tilt street crossfall");
  s.houseWeight = 0;
  s.workshopWeight = 0;
  s.gardenWeight = 1;
  bool gardens = generateCityLayout(d, {}, a, g, s, flat, c, &e);
  for (auto &o : c.objects)
    gardens &= o.kind == CityObjectKind::Garden || o.kind == CityObjectKind::RejectedGap;
  check(gardens, "weighted recipe can reserve only gardens");
  auto namedArtery = a;
  namedArtery[0].id = 1000;
  auto namedGate = g;
  namedGate[0].externalRoad = 1000;
  bool unique = generateCityLayout(d, {}, namedArtery, namedGate, s, flat, c, &e);
  std::set<uint64_t> streetIds;
  for (const auto &street : c.streets)
    unique &= streetIds.insert(street.id).second;
  check(unique, "generated street IDs cannot collide with fixed artery IDs");
  Poly thin{{96.74798838836085, 0, 40.301558}, {97.05388472061601, 0, 40.90467529022667}, {99.20185381906018, 0, 45.139696}, {75, 0, 45.139696}, {75, 0, 40.301558}};
  BoundaryRegistry registry;
  std::vector<BoundaryInput> boundaries;
  PatchDomain patch;
  patch.id = 1;
  for (size_t i = 0; i < thin.size(); ++i)
  {
    boundaries.push_back({i + 1, {i + 1, thin[i]}, {(i + 1) % thin.size() + 1, thin[(i + 1) % thin.size()]}, {}});
    patch.outer.push_back({i + 1, i + 1 == thin.size()});
  }
  PatchMesh thinMesh;
  check(registry.commit(boundaries, &e) && compilePatchMesh(patch, registry, {}, flat, thinMesh, &e) && thinMesh.constraints.size() == 5, "near-collinear boundary parity retains every CDT constraint");
  std::cout << "CITY_TESTS assertions=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
