#include "json.hpp"
#include "procedural/world/MapPartition.hpp"
#include "procedural/world/WorldCityTerritory.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace procedural::world::rewrite;
using nlohmann::json;
using Poly = std::vector<Position>;
Poly rect(double x, double z, double w, double h)
{
  return {{x, 0, z}, {x + w, 0, z}, {x + w, 0, z + h}, {x, 0, z + h}};
}
double area(const Poly &p)
{
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
    a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x;
  return a * .5;
}
json poly(const Poly &p)
{
  json a = json::array();
  for (auto v : p)
    a.push_back({v.x, v.y, v.z});
  return a;
}
json encode(const CityTerritory &p)
{
  json j;
  j["revision"] = p.revision;
  j["suitabilityFingerprint"] = p.suitabilityFingerprint;
  j["cropArea"] = p.cropArea;
  j["farmyardArea"] = p.farmyardArea;
  j["rejections"] = p.rejections;
  j["regions"] = json::array();
  j["patches"] = json::array();
  j["holdings"] = json::array();
  j["contours"] = json::array();
  for (const auto &d : p.regions.districts)
  {
    json pieces = json::array();
    for (auto q : d.pieces)
      pieces.push_back(poly(q));
    j["regions"].push_back({{"id", d.id}, {"use", districtUseName(d.use)}, {"pieces", pieces}, {"access", poly(d.access)}, {"area", d.area}, {"shortfall", d.unmetTargetArea}});
  }
  for (size_t i = 0; i < p.rural.patches.size(); ++i)
  {
    auto &r = p.rural.patches[i];
    json fields = json::array();
    for (auto &f : p.fields[i].fields)
      fields.push_back({{"boundary", poly(f.boundary)}, {"access", poly(f.access)}, {"area", f.area}});
    j["patches"].push_back(
        {{"id", r.id},
         {"source", r.source},
         {"originalRegion", r.originalRegion},
         {"boundaryAccessStart", r.boundaryAccessStart},
         {"boundary", poly(r.boundary)},
         {"access", poly(r.access)},
         {"area", r.area},
         {"fields", fields},
         {"cropArea", p.fields[i].cropArea},
         {"trackArea", p.fields[i].trackArea},
         {"marginArea", p.fields[i].marginArea}});
  }
  for (auto &h : p.holdings)
  {
    json buildings = json::array();
    for (auto &b : h.buildings)
      buildings.push_back({{"id", b.id}, {"use", int(b.use)}, {"footprint", poly(b.footprint)}, {"entrance", poly({b.entrance})}, {"height", b.height}});
    j["holdings"].push_back({{"patch", h.patch}, {"source", h.source}, {"yard", poly(h.yard)}, {"access", poly(h.access)}, {"buildings", buildings}});
  }
  for (auto c : p.surfaces.contours)
    j["contours"].push_back({{"id", c.id}, {"material", c.material}, {"role", int(c.role)}, {"loop", poly(c.loop)}});
  return j;
}
int main(int argc, char **argv)
{
  int checks = 0, failures = 0;
  auto check = [&](bool ok, const std::string &message)
  {
    ++checks;
    failures += !ok;
    std::cout << (ok ? "PASS " : "FAIL ") << message << std::endl;
  };
  std::string error;
  auto flat = [](double, double)
  {
    return 0.;
  };
  std::filesystem::path output = argc > 1 ? argv[1] : "";
  if (!output.empty())
    std::filesystem::create_directories(output);
  // Large estate ownership spans multiple former local street blocks.
  CityPolygon domain{rect(0, 0, 320, 240), {}};
  CitySettings city;
  city.recipe = CityRecipe::PlannedBlocks;
  city.blockHousing = true;
  city.targetBlockArea = 1500;
  city.targetPlotArea = 650;
  city.streetCurvature = 0;
  city.housingAreas = {{80, DistrictUse::WealthyResidential, {domain.outer}, 650, false, .55}};
  std::vector<CityStreet> arteries{{1, {0, 0, 120}, {320, 0, 120}, 4, true}};
  CityLayout before, estate;
  check(generateCityLayout(domain, {}, arteries, {{1, {0, 0, 120}, 1}}, city, flat, before, &error), "estate baseline: " + error);
  city.housingAreas[0].reclaimLocalStreets = true;
  check(generateCityLayout(domain, {}, arteries, {{1, {0, 0, 120}, 1}}, city, flat, estate, &error), "estate reclaims eligible local streets: " + error);
  check(!estate.reclaimedStreets.empty() && estate.blocks.size() < before.blocks.size(), "estate aggregation produces fewer, larger physical blocks");
  bool garden = !estate.objects.empty(), mainRoute = false;
  for (auto &o : estate.objects)
  {
    double shell = 0;
    for (auto &part : o.parts)
      shell += area(part.footprint);
    garden &= shell <= area(o.footprint) * .45 + 1e-5;
  }
  for (auto &road : estate.streets)
    mainRoute |= road.id == 1 && road.arterial && road.a.x == 0 && road.b.x == 320;
  check(garden && mainRoute, "wealthy plots retain gardens and the immutable main route");
  if (!output.empty())
  {
    json j;
    j["envelope"] = poly(estate.envelope.outer);
    j["reclaimed"] = estate.reclaimedStreets;
    j["beforeBlocks"] = before.blocks.size();
    j["afterBlocks"] = estate.blocks.size();
    j["streets"] = json::array();
    j["buildings"] = json::array();
    j["contours"] = json::array();
    for (auto &r : estate.streets)
      j["streets"].push_back({{"path", poly(r.path)}, {"width", r.halfWidth}, {"arterial", r.arterial}});
    for (auto &o : estate.objects)
    {
      json parts = json::array();
      for (auto &part : o.parts)
        parts.push_back(poly(part.footprint));
      j["buildings"].push_back({{"plot", poly(o.footprint)}, {"parts", parts}, {"courtyard", poly(o.courtyard)}});
    }
    for (auto &c : estate.contours)
      j["contours"].push_back({{"loop", poly(c.loop)}, {"material", c.material}});
    std::ofstream(output / "wealthy-estate.json") << j.dump(2);
  }
  // Multi-cell military compounds no longer leave artificial inset seams.
  city.housingAreas.clear();
  city.districts = {{90, 1, DistrictUse::Military, {1, 2}, {rect(40, 20, 80, 60), rect(120, 20, 80, 60)}, {}, {}, 9600, 0}};
  CityLayout military;
  check(generateCityLayout(domain, {}, arteries, {{1, {0, 0, 120}, 1}}, city, flat, military, &error), "multi-cell military compound: " + error);
  check(military.reservations.size() == 1, "adjacent military cells merge before compound setback");
  auto invalidMilitary = city;
  invalidMilitary.districts[0].pieces = {rect(40, 80, 160, 80)};
  CityLayout protectedCity;
  protectedCity.revision = 99;
  check(!generateCityLayout(domain, {}, arteries, {{1, {0, 0, 120}, 1}}, invalidMilitary, flat, protectedCity, &error) && protectedCity.revision == 99, "compound expansion cannot consume the fixed main route");
  auto workers = city;
  workers.districts.clear();
  workers.housingAreas = {{81, DistrictUse::WorkerHousing, {domain.outer}, 140, false, 0}};
  CityLayout workerQuarter;
  bool workersOK = generateCityLayout(domain, {}, arteries, {{1, {0, 0, 120}, 1}}, workers, flat, workerQuarter, &error);
  check(
      workersOK && std::any_of(
                       workerQuarter.objects.begin(),
                       workerQuarter.objects.end(),
                       [](const auto &o)
                       {
                         return o.buildingType == CityBuildingType::WorkerLodging;
                       }),
      "worker quarter assigns lodging buildings for future occupancy planning: " + error);
  // Terrain-driven territory around a reserved city; public world builder consumes the same batch type.
  WorldSplineLandforms forms;
  WorldPhysicalTerrain physical;
  WorldRiverChannels rivers;
  check(forms.build({}, {}, &error) && physical.build(forms, flat, {}, {}, {}, &error), "territory terrain dependencies");
  SuitabilitySettings ss;
  ss.minX = -400;
  ss.minZ = -320;
  ss.maxX = 1200;
  ss.maxZ = 960;
  ss.spacingMetres = 16;
  CityTerritoryInput input;
  input.producer = 700;
  input.revision = 7;
  SurfaceContour urban;
  urban.id = 1;
  urban.material = 11;
  urban.loop = rect(-80, -80, 720, 624);
  input.occupied = {urban};
  WorldContourBatch citySurface{702, 20, {}};
  json cityReference;
  if (argc > 2)
  {
    std::ifstream source(argv[2]);
    source >> cityReference;
    auto decode = [](const json &ring)
    {
      Poly p;
      for (auto v : ring)
        p.push_back({v[0], v[1], v[2]});
      return p;
    };
    input.occupied.clear();
    auto reserve = [&](const json &ring)
    {
      SurfaceContour c;
      c.id = input.occupied.size() + 1;
      c.material = 11;
      c.loop = decode(ring);
      input.occupied.push_back(c);
    };
    reserve(cityReference["envelope"]);
    for (auto ring : cityReference["suburbs"])
      reserve(ring);
    for (auto tower : cityReference.value("towers", json::array()))
      reserve(tower["footprint"]);
    for (auto source : cityReference["contours"])
    {
      SurfaceContour c;
      c.id = source["id"];
      c.material = source["material"];
      c.loop = decode(source["loop"]);
      if (c.material == 1)
        c.role = SurfaceRole::Road;
      citySurface.contours.push_back(c);
    }
  }
  input.ruralSources = {{1, {-120, 0, 152}, 60000}, {2, {680, 0, 168}, 60000}, {3, {376, 0, 584}, 60000}};
  std::vector<Position> exits;
  for (auto s : input.ruralSources)
    exits.push_back(s.exit);
  WorldSuitability world;
  check(
      world.build(
          physical,
          rivers,
          [](double, double)
          {
            return .75;
          },
          [](double, double)
          {
            return .4;
          },
          exits,
          ss,
          7,
          &error),
      "territory frozen terrain suitability");
  // Coarse 80 m regions cover 2 km², independently of the wall's building resolution.
  for (int z = 0; z < 16; ++z)
    for (int x = 0; x < 20; ++x)
    {
      CityPlanCell c;
      c.id = 1 + z * 20 + x;
      c.boundary = rect(-400 + x * 80, -320 + z * 80, 80, 80);
      c.fertility = .75;
      c.secure = true;
      c.defense = .7;
      c.outsideOldWall = true;
      input.cells.push_back(c);
    }
  input.gateCells = {104, 134, 230};
  input.districts = {
    {1, DistrictUse::WealthyResidential, 12800, 38400, true}, {2, DistrictUse::WorkerHousing, 12800, 38400, true}, {3, DistrictUse::Military, 19200, 51200, true}, {4, DistrictUse::Suburb, 25600, 102400, true}};
  CityTerritorySettings settings;
  settings.rural.maximumArea = 12000;
  settings.rural.minimumArea = 3000;
  settings.rural.minimumWidth = 48;
  settings.rural.maximumPatches = 8;
  settings.rural.maximumAccessCost = 1600;
  settings.rural.patchSpacing = 32;
  settings.fields.targetArea = 2200;
  settings.fields.maximumArea = 4000;
  // Use the SAME Voronoi implementation as the world map. Runtime callers pass
  // their existing region IDs/polygons instead of generating another partition.
  std::vector<procedural::Point2> sites;
  for (int z = 0; z < 6; ++z)
    for (int x = 0; x < 8; ++x)
      sites.push_back({-300. + x * 200 + 37 * std::sin(x * 7 + z * 3), -220. + z * 210 + 31 * std::cos(x * 3 + z * 5)});
  std::vector<procedural::world::VoronoiCell> original;
  check(procedural::world::buildVoronoiCells(sites, {{-400, -320}, {1200, -320}, {1200, 960}, {-400, 960}}, .001, original, &error), "original world-map Voronoi partition: " + error);
  for (size_t i = 0; i < original.size(); ++i)
  {
    RuralRegion region;
    region.id = 1000 + i;
    for (auto p : original[i].area)
      region.boundary.push_back({p.x, 0, p.y});
    if (area(region.boundary) < 0)
      std::reverse(region.boundary.begin(), region.boundary.end());
    if (i == 7 || i == 15)
      region.cover = RuralLandCover::Forest;
    if (i == 40)
      region.cover = RuralLandCover::Rock;
    settings.rural.regions.push_back(region);
  }
  for (auto &source : input.ruralSources)
    source.areaBudget = 500000;
  settings.rural.maximumPatches = 32;
  settings.rural.minimumArea = 10000;
  settings.rural.maximumArea = 80000;
  settings.fields.targetArea = 12000;
  settings.fields.maximumArea = 20000;
  settings.fields.entranceWidth = 4;
  settings.fields.trackWidth = 6;
  settings.fields.headland = 6;
  settings.farmsteadInterval = 4;
  CityTerritory result;
  bool ok = generateCityTerritory(input, world, settings, flat, result, &error);
  check(ok, "regional allocations, fields and farmsteads: " + error);
  if (ok)
  {
    check(result.regions.districts.size() == 4 && result.regions.assignedArea >= 200000, "large exterior urban, wealthy, worker and military regions allocated");
    check(result.rural.patches.size() >= 4 && result.cropArea > 10000 && !result.holdings.empty(), "farmland contains fields, farmhouses, barns and worker cottages");
    bool originals = true;
    for (auto &p : result.rural.patches)
    {
      auto it = std::find_if(
          settings.rural.regions.begin(),
          settings.rural.regions.end(),
          [&](auto &r)
          {
            return r.id == p.originalRegion;
          });
      originals &= it != settings.rural.regions.end() && it->cover == RuralLandCover::Open && poly(it->boundary) == poly(p.boundary);
    }
    check(originals && result.rural.patchArea > (argc > 2 ? 300000 : 80000), "broad farmland preserves original open Voronoi cells and excludes forest/rock");
    bool disjoint = true, ledger = true;
    for (size_t i = 0; i < result.rural.patches.size(); ++i)
    {
      auto &p = result.rural.patches[i];
      double yards = 0;
      for (auto &h : result.holdings)
        if (h.patch == p.id)
          yards += h.yardArea;
      auto &f = result.fields[i];
      ledger &= std::abs(p.area - (f.cropArea + f.trackArea + f.marginArea + yards)) < 1e-5;
      for (auto &field : f.fields)
      {
        for (auto &region : result.regions.districts)
          for (auto &piece : region.pieces)
            if (cityPolygonsOverlap(field.boundary, piece))
            {
              disjoint = false;
              std::fprintf(stderr, "field of patch %llu (region %llu) overlaps district %llu use %d\n", (unsigned long long)p.id, (unsigned long long)p.originalRegion, (unsigned long long)region.id, int(region.use));
            }
        for (auto &h : result.holdings)
          if (cityPolygonsOverlap(field.boundary, h.yard))
          {
            disjoint = false;
            std::fprintf(stderr, "field of patch %llu overlaps a farmyard of patch %llu\n", (unsigned long long)p.id, (unsigned long long)h.patch);
          }
      }
    }
    check(disjoint, "fields exclude settled regions and farmyards");
    check(ledger, "fields keep a balanced area ledger");
    auto permuted = input;
    std::reverse(permuted.cells.begin(), permuted.cells.end());
    std::reverse(permuted.ruralSources.begin(), permuted.ruralSources.end());
    CityTerritory repeat;
    check(generateCityTerritory(permuted, world, settings, flat, repeat, &error) && encode(result) == encode(repeat), "territory is deterministic under source/cell permutation");
    SurfaceMapSettings bounds;
    bounds.minX = ss.minX;
    bounds.minZ = ss.minZ;
    bounds.maxX = ss.maxX;
    bounds.maxZ = ss.maxZ;
    bounds.maximumFaces = 20000;
    bounds.cutsX = {-96, 640}; // Shared district columns contain city support cuts.
    SurfaceRefinement refine;
    refine.groundSpacingMetres = 100;
    refine.modifiedSpacingMetres = 24;
    WorldFinalPatches frozen;
    WorldContourBatch occupied{701, 20, input.occupied};
    auto batches = std::vector<WorldContourBatch>{result.surfaces, occupied};
    if (!citySurface.contours.empty())
    {
      // The complete city already owns its ground. Occupancy masks only guide
      // rural selection; republishing them adds unnecessary arrangement seams.
      batches = {result.surfaces, citySurface};
    }
    bool frozenOK = freezeWorldPatches(batches, bounds, refine, flat, 7, frozen, &error);
    check(frozenOK, "territory contours freeze through the actual world patch builder: " + error);
    if (frozenOK)
      check(std::abs(frozen.mesh.groundArea - 1600 * 1280) < .01, "world builder preserves full territory coverage");
    if (!output.empty())
    {
      auto j = encode(result);
      j["originalRegions"] = json::array();
      for (auto &r : settings.rural.regions)
        j["originalRegions"].push_back({{"id", r.id}, {"boundary", poly(r.boundary)}, {"cover", int(r.cover)}});
      j["bounds"] = {ss.minX, ss.minZ, ss.maxX, ss.maxZ};
      j["occupied"] = json::array();
      for (auto c : input.occupied)
        j["occupied"].push_back(poly(c.loop));
      if (!cityReference.is_null())
        j["city"] = cityReference;
      j["frozenFaces"] = frozen.map.faces.size();
      j["frozenContours"] = result.surfaces.contours.size() + (citySurface.contours.empty() ? occupied.contours.size() : citySurface.contours.size());
      j["suitabilitySource"] = "synthetic flat 16 m terrain; outer districts are allocations, not engineered urban roads";
      std::ofstream(output / "city-territory.json") << j.dump(2);
    }
    std::cout << "TERRITORY_METRIC regions=" << result.regions.districts.size() << " urbanArea=" << result.regions.assignedArea << " patches=" << result.rural.patches.size() << " farms=" << result.holdings.size()
              << " cropArea=" << result.cropArea << " contours=" << result.surfaces.contours.size() << std::endl;
  }
  CityTerritory sentinel;
  sentinel.revision = 99;
  input.producer = 0;
  check(!generateCityTerritory(input, world, settings, flat, sentinel, &error) && sentinel.revision == 99, "invalid territory leaves prior product unchanged");
  std::cout << "TERRITORY_TESTS assertions=" << checks << " failures=" << failures << std::endl;
  return failures ? 1 : 0;
}
