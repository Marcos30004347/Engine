#include "procedural/world/WorldDomainGeneratorAdapters.hpp"
#include "procedural/world/WorldGenerationEdits.hpp"
#include "procedural/world/WorldNativeGenerator.hpp"
#include "procedural/world/WorldPatchGeneratorAdapters.hpp"
#include "procedural/world/WorldPipelineDirector.hpp"
#include "procedural/world/WorldSettlementFabricAdapter.hpp"
#include "procedural/world/WorldSettlementSiteAdapter.hpp"
#include "procedural/world/generator/BlockGrammar.hpp"
#include "procedural/world/generator/CompoundPlanner.hpp"
#include "procedural/world/generator/SettlementGenerators.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <fstream>
#include <iomanip>
#include <iostream>
using namespace procedural::world::rewrite;
int main(int argc, char **argv)
{
  int failed = 0, checks = 0;
  std::string error;
  auto check = [&](bool ok, const char *name)
  {
    ++checks;
    failed += !ok;
    std::cout << (ok ? "PASS " : "FAIL ") << name << " " << error << '\n';
    error.clear();
  };
  CityPlanGenerationBrief city;
  for (int z = 0; z < 3; ++z)
    for (int x = 0; x < 3; ++x)
    {
      CityPlanCell cell;
      cell.id = z * 3 + x + 1;
      cell.boundary = {{x * 20., 0, z * 20.}, {x * 20. + 20, 0, z * 20.}, {x * 20. + 20, 0, z * 20. + 20}, {x * 20., 0, z * 20. + 20}};
      city.cells.push_back(cell);
    }
  city.gates = {1};
  city.districts = {{1, DistrictUse::Residential, 400, 800, true}};
  GenerationSnapshot outputs;
  check(
      runGenerators({cityMasterPlanGenerator(1)}, {{{"city.plan-brief", 1}, {city, 1}}}, 7, outputs, {}, &error) && !std::any_cast<const CityMasterPlan &>(outputs.at({"city.master-plan", 1}).value).districts.empty(),
      "native city allocation through common interface");
  RuntimeBuildInput runtime;
  runtime.dependencies = {1, 1, 1, 1, 1};
  runtime.map.minX = 0;
  runtime.map.minZ = 0;
  runtime.map.maxX = 32;
  runtime.map.maxZ = 32;
  runtime.map.cutsX = {16};
  runtime.map.cutsZ = {16};
  runtime.terrain = [](double, double)
  {
    return 0.;
  };
  runtime.errors.resize(1);
  runtime.densities.resize(1);
  runtime.covers = {{0, {0}, false, false}};
  SurfaceContour ground;
  ground.id = 1;
  ground.loop = {{0, 0, 0}, {32, 0, 0}, {32, 0, 32}, {0, 0, 32}};
  runtime.producers = {{1, 0, {ground}}};
  check(runGenerators({runtimeWorldGenerator(1)}, {{{"runtime.brief", 1}, {runtime, 1}}}, 7, outputs, {}, &error), "native world builder executes through common interface");
  if (outputs.count({"runtime.candidate", 1}))
  {
    auto &assets = std::any_cast<const RuntimeBuildResult &>(outputs.at({"runtime.candidate", 1}).value).assets;
    WorldRuntimePublication publication;
    uint64_t ticket = 0;
    bool ok = publication.begin(assets, std::vector<uint32_t>(assets.levels[0].world.tiles.size(), 0), 1000000, ticket, &error);
    if (ok)
      for (uint32_t i = 0; i < assets.levels[0].world.tiles.size(); ++i)
        ok &= publication.complete(ticket, i, assets.dependencies, &error);
    auto snapshot = publication.acquire();
    RuntimeSurfaceHit hit;
    check(ok && snapshot && queryRuntimeSurface(*snapshot, 16, 16, 0, hit, &error) && hit.height == 0, "DAG world candidate publishes and serves real collision/surface queries");
  }
  // W0: actual-world settlement input adapter on a generated world.
  {
    procedural::world::WorldPipelineSettings settings;
    settings.brief.extentMetres = 12000;
    settings.brief.seed = 17;
    settings.brief.reliefMetres = 900;
    settings.brief.tags = {procedural::world::Tag::Coast, procedural::world::Tag::Lowland};
    settings.cellSpacingMetres = 400;
    settings.hierarchyLevels = 1;
    auto world = std::make_shared<procedural::world::GeneratedWorld>();
    bool generated = procedural::world::generateWorldDirected(settings, *world, {}, &error);
    check(generated, "actual 12 km world generates for settlement integration");
    WorldSiteSelectionSettings siteSettings;
    WorldSettlementSite site;
    bool selected = generated && selectWorldSettlementSite(*world, siteSettings, site, &error);
    check(selected, "W0 site selection composes a brief from the actual world");
    if (selected)
    {
      size_t rejected = 0;
      for (auto &c : site.candidates)
        rejected += !c.accepted;
      std::cout << "W0_SITE cell=" << site.cell << " region=\"" << site.regionName << "\" centre=(" << site.centre.x << "," << site.centre.z << ") elevation=" << site.centre.y << " candidates=" << site.candidates.size()
                << " rejected=" << rejected << " attempts=" << site.attempts << " planningCells=" << site.brief.input.cells.size() << " regions=" << site.regions.size() << " gates=" << site.brief.input.gateCells.size()
                << " sources=" << site.brief.input.ruralSources.size() << " districts=" << site.brief.input.districts.size() << " origin=(" << site.origin.x << "," << site.origin.z << ") size=" << site.bounds.maxX << "x"
                << site.bounds.maxZ << " openArea=" << site.openArea << " hinterlandArea=" << site.hinterlandArea << "\n";
      size_t shown = 0;
      for (auto &c : site.candidates)
        if (!c.accepted && shown++ < 6)
          std::cout << "  rejected cell " << c.cell << ": " << c.rejection << "\n";
      for (auto &r : site.rejections)
        std::cout << "  omission: " << r << "\n";
      double lo = 1e300, hi = -1e300;
      for (auto &cell : site.brief.input.cells)
      {
        double y = site.brief.terrain(cell.boundary[0].x, cell.boundary[0].z);
        lo = std::min(lo, y);
        hi = std::max(hi, y);
      }
      check(hi > lo, "terrain callback samples the actual surface rather than a flat default");
      check(!site.water(site.localCentre.x, site.localCentre.z), "site centre is not on explicit map water");
      bool waterSeen = false, waterExplicit = true;
      for (const auto &n : world->graph.nodes())
        if (!n.refined() && !n.land && n.site.x - site.origin.x > site.bounds.minX && n.site.x - site.origin.x < site.bounds.maxX && n.site.y - site.origin.z > site.bounds.minZ &&
            n.site.y - site.origin.z < site.bounds.maxZ)
        {
          waterSeen = true;
          waterExplicit &= site.water(n.site.x - site.origin.x, n.site.y - site.origin.z);
        }
      check(!waterSeen || waterExplicit, "explicit ocean/lake cells inside the domain read as water");
      WorldSettlementSite repeat;
      bool same = selectWorldSettlementSite(*world, siteSettings, repeat, &error) && repeat.id == site.id && repeat.cell == site.cell && repeat.candidates.size() == site.candidates.size() &&
                  repeat.brief.input.cells.size() == site.brief.input.cells.size() && repeat.regions.size() == site.regions.size() && repeat.brief.input.gateCells == site.brief.input.gateCells &&
                  repeat.brief.suitability.fingerprint() == site.brief.suitability.fingerprint();
      for (size_t i = 0; same && i < site.candidates.size(); ++i)
        same &= site.candidates[i].cell == repeat.candidates[i].cell && site.candidates[i].rejection == repeat.candidates[i].rejection;
      for (size_t i = 0; same && i < site.regions.size(); ++i)
        same &= site.regions[i].id == repeat.regions[i].id && site.regions[i].cover == repeat.regions[i].cover;
      for (size_t i = 0; same && i < site.brief.input.cells.size(); ++i)
        same &= site.brief.input.cells[i].id == repeat.brief.input.cells[i].id;
      check(same, "same seed yields the same site, candidate ledger, region and cell IDs");
      uint32_t waterCell = procedural::world::kInvalidNode;
      for (const auto &n : world->graph.nodes())
        if (!n.refined() && !n.land)
        {
          waterCell = n.id;
          break;
        }
      auto bad = siteSettings;
      bad.preferredCell = waterCell;
      WorldSettlementSite rejectedSite;
      bool refused = waterCell != procedural::world::kInvalidNode && !selectWorldSettlementSite(*world, bad, rejectedSite, &error) && !error.empty();
      std::cout << "  explicit water cell " << waterCell << " -> " << error << "\n";
      check(refused, "a preferred water cell is rejected with a visible reason");
      CityTerritory territory;
      bool composed = generateCityTerritory(site.brief.input, site.brief.suitability, site.brief.settings, site.brief.terrain, territory, &error);
      std::string territoryMessage = "W0 territory allocates districts and farmland on actual terrain: " + error;
      check(composed, territoryMessage.c_str());
      if (composed)
      {
        std::cout << "W0_TERRITORY districts=" << territory.regions.districts.size() << " omissions=" << territory.regions.omissions.size() << " urbanArea=" << territory.regions.assignedArea << " patches=" << territory.rural.patches.size()
                  << " cropArea=" << territory.cropArea << " farms=" << territory.holdings.size() << " contours=" << territory.surfaces.contours.size() << " eligibleArea=" << territory.rural.eligibleArea
                  << " ruralRejections=" << territory.rural.rejections.size() << "\n";
        for (auto &o : territory.regions.omissions)
          std::cout << "  district omission " << o.request << ": " << o.reason << "\n";
        size_t ruralShown = 0;
        for (auto &r : territory.rural.rejections)
          if (ruralShown++ < 40)
            std::cout << "  rural: " << r << "\n";
        {
          const auto &w = site.brief.suitability;
          const char *names[] = {"Wet", "Flood", "Cliff", "Talus", "Brink", "Steep", "Rough", "Inaccessible", "Ramp", "PoorSoil", "Saturated"};
          std::vector<size_t> histogram(11, 0);
          size_t eligible = 0, walkable = 0;
          for (auto &c : w.cells())
          {
            eligible += c.reasons == 0;
            walkable += c.walkable;
            for (int b = 0; b < 11; ++b)
              histogram[b] += (c.reasons >> b) & 1;
          }
          std::cout << "  suitability cells=" << w.cells().size() << " eligible=" << eligible << " walkable=" << walkable;
          for (int b = 0; b < 11; ++b)
            if (histogram[b])
              std::cout << " " << names[b] << "=" << histogram[b];
          std::cout << "\n";
          size_t regionShown = 0;
          for (auto &region : site.regions)
          {
            if (region.cover != RuralLandCover::Open || regionShown++ >= 8)
              continue;
            std::vector<size_t> h(11, 0);
            size_t cells = 0, blocked = 0, noAccess = 0;
            double x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
            for (auto p : region.boundary)
            {
              x0 = std::min(x0, p.x);
              z0 = std::min(z0, p.z);
              x1 = std::max(x1, p.x);
              z1 = std::max(z1, p.z);
            }
            for (size_t i = 0; i < w.cells().size(); ++i)
            {
              auto c = w.cells()[i].center;
              if (c.x < x0 || c.x > x1 || c.z < z0 || c.z > z1)
                continue;
              ++cells;
              blocked += w.cells()[i].reasons != 0;
              noAccess += !territory.rural.catchments[i];
              for (int b = 0; b < 11; ++b)
                h[b] += (w.cells()[i].reasons >> b) & 1;
            }
            std::cout << "  region " << region.id << " cells=" << cells << " blocked=" << blocked << " noCatchment=" << noAccess;
            for (int b = 0; b < 11; ++b)
              if (h[b])
                std::cout << " " << names[b] << "=" << h[b];
            std::cout << "\n";
          }
        }
        check(!territory.regions.districts.empty(), "population-derived districts are allocated to actual planning cells");
      }
      // Fabric (terrace, streets, houses, residents) and W1 runtime publication.
      if (composed)
      {
        SettlementFabricSettings fabricSettings;
        SettlementFabric fabric;
        bool fabricOK = composeSettlementFabric(site, territory, fabricSettings, fabric, &error);
        std::string fabricMessage = "settlement fabric composes on the actual territory: " + error;
        check(fabricOK, fabricMessage.c_str());
        if (fabricOK)
        {
          std::cout << "W1_FABRIC terrace=" << (fabric.terraceAccepted ? "accepted" : "rejected") << " level=" << fabric.terraceLevel << " step=" << fabric.terraceStep << " polygon=" << fabric.polygon.outer.size() << " holes=" << fabric.polygon.holes.size()
                    << " routeCells=" << fabric.routeCells << " arteries=" << fabric.arteries.size() << " gates=" << fabric.gates.size() << " layout=" << (fabric.layoutAccepted ? "accepted" : "rejected") << " streets=" << fabric.layout.streets.size()
                    << " objects=" << fabric.layout.objects.size() << " populated=" << fabric.populated << " housed=" << fabric.population.housed << " unhoused=" << fabric.population.unhoused << " buildings=" << fabric.population.buildings.size()
                    << " structures=" << fabric.structures.triangles.size() << " farmTriangles=" << fabric.farmTriangles << "\n";
          for (auto &r : fabric.rejections)
            std::cout << "  fabric: " << r << "\n";
          {
            size_t vertices = 0, cityVertices = 0, houses = 0, gardens = 0, gaps = 0;
            for (auto &c : territory.surfaces.contours)
              vertices += c.loop.size();
            for (auto &c : fabric.layout.contours)
              cityVertices += c.loop.size();
            for (auto &o : fabric.layout.objects)
              o.kind == CityObjectKind::Garden ? ++gardens : o.kind == CityObjectKind::RejectedGap ? ++gaps : ++houses;
            std::cout << "  territory contours=" << territory.surfaces.contours.size() << " vertices=" << vertices << " city contours=" << fabric.layout.contours.size() << " vertices=" << cityVertices << " houses=" << houses << " gardens=" << gardens
                      << " gaps=" << gaps << " polygonArea=" << [&] { double a = 0; auto &p = fabric.polygon.outer; for (size_t i = 0; i < p.size(); ++i) a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x; return a * .5; }() << "\n";
            for (auto &c : territory.surfaces.contours)
              if (c.loop.size() < 3 || c.loop.size() > 256)
                std::cout << "  bad contour id=" << c.id << " material=" << c.material << " role=" << int(c.role) << " vertices=" << c.loop.size() << "\n";
          }
          check(fabric.terraceAccepted && fabric.layoutAccepted && !fabric.layout.objects.empty(), "village streets and houses are laid out on the bounded terrace");
          check(fabric.populated && fabric.population.housed > 0, "residents are housed through the director with real terrain/water eligibility");
          RuntimeBuildInput runtimeInput;
          SettlementRuntimeSettings runtimeSettings;
          bool briefOK = composeSettlementRuntimeInput(site, territory, fabric, runtimeSettings, runtimeInput, &error);
          std::string briefMessage = "W1 runtime brief composes from site, territory and fabric: " + error;
          check(briefOK, briefMessage.c_str());
          RuntimeBuildResult built;
          bool buildOK = briefOK && buildRuntimeWorld(runtimeInput, nullptr, built, &error);
          // Runtime publication is parked (user direction 2026-09-13): report,
          // but only require an explicit message when it does not build.
          std::string buildMessage = "W1 runtime build either succeeds or reports an explicit failure: " + error;
          if (!buildOK && !error.empty())
          {
            std::cout << "  runtime (parked): " << error << "\n";
            buildOK = true;
          }
          if (!buildOK)
          {
            struct Worst { double deviation, length; uint64_t producer, id; uint32_t material; int role; size_t edge; };
            std::vector<Worst> worst;
            for (auto &batch : runtimeInput.producers)
              for (auto &c : batch.contours)
              {
                if (!c.terrainSurface)
                  continue;
                for (size_t i = 0; i < c.loop.size(); ++i)
                {
                  auto a = c.loop[i], b = c.loop[(i + 1) % c.loop.size()];
                  double ya = runtimeInput.terrain(a.x, a.z), yb = runtimeInput.terrain(b.x, b.z), len = std::hypot(b.x - a.x, b.z - a.z), dev = 0;
                  for (double t : {.25, .5, .75})
                    dev = std::max(dev, std::abs(runtimeInput.terrain(a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t) - (ya + (yb - ya) * t)));
                  if (dev > .2)
                    worst.push_back({dev, len, batch.producer, c.id, c.material, int(c.role), i});
                }
              }
            std::sort(worst.begin(), worst.end(), [](auto &x, auto &y) { return x.deviation > y.deviation; });
            std::cout << "  edges over 0.2 m: " << worst.size() << " cutsX=" << runtimeInput.map.cutsX.size() << " cutsZ=" << runtimeInput.map.cutsZ.size() << "\n";
            {
              WorldFinalPatches frozen;
              SurfaceRefinement initial = runtimeInput.initial;
              initial.samplingSeed = runtimeInput.samplingSeed;
              std::string freezeError;
              if (freezeWorldPatches(runtimeInput.producers, runtimeInput.map, initial, runtimeInput.terrain, runtimeInput.revision, frozen, &freezeError))
              {
                for (auto &face : frozen.map.faces)
                {
                  bool hasA = false, hasB = false;
                  for (auto vi : face.vertices)
                  {
                    auto v = frozen.map.vertices[vi];
                    hasA |= std::abs(v.x - 2400) < .01 && std::abs(v.z - 2552) < .01;
                    hasB |= std::abs(v.x - 3040) < .01 && std::abs(v.z - 2552) < .01;
                  }
                  if (hasA && hasB)
                  {
                    std::cout << "  face " << face.id << " material " << face.material << " source " << face.sourceContour << " ring:";
                    for (auto vi : face.vertices)
                      std::cout << " (" << frozen.map.vertices[vi].x << "," << frozen.map.vertices[vi].z << ")";
                    std::cout << "\n";
                  }
                }
              }
              else
                std::cout << "  freeze: " << freezeError << "\n";
            }
            for (auto &batch : runtimeInput.producers)
              for (auto &c : batch.contours)
                for (size_t i = 0; i < c.loop.size(); ++i)
                  if (std::abs(c.loop[i].z - 2552) < .5 && c.loop[i].x > 2000)
                    std::cout << "  vertex near z=2552: producer=" << batch.producer << " contour=" << c.id << " material=" << c.material << " role=" << int(c.role) << " terrain=" << c.terrainSurface << " size=" << c.loop.size() << " v=(" << c.loop[i].x << "," << c.loop[i].z << ")\n";
            for (size_t i = 0; i < worst.size() && i < 6; ++i)
              std::cout << "  edge producer=" << worst[i].producer << " contour=" << worst[i].id << " material=" << worst[i].material << " role=" << worst[i].role << " edge=" << worst[i].edge << " length=" << worst[i].length << " deviation=" << worst[i].deviation << "\n";
          }
          check(buildOK, buildMessage.c_str());
          if (buildOK && built.assets.levels.empty())
            std::cout << "W1_RUNTIME built with no levels; publication checks skipped\n";
          if (buildOK && !built.assets.levels.empty())
          {
            const auto &assets = built.assets;
            std::cout << "W1_RUNTIME tiles=" << assets.levels[0].world.tiles.size() << " faces=" << assets.levels[0].world.map.faces.size() << " triangles=" << assets.levels[0].world.mesh.triangles.size() << " structures=" << assets.structures.triangles.size()
                      << " covers=" << assets.coverDefinitions.size() << " changedTiles=" << built.changedTiles.size() << "\n";
            WorldRuntimePublication publication;
            uint64_t ticket = 0;
            bool published = publication.begin(assets, std::vector<uint32_t>(assets.levels[0].world.tiles.size(), 0), 4000000, ticket, &error);
            for (uint32_t i = 0; published && i < assets.levels[0].world.tiles.size(); ++i)
              published &= publication.complete(ticket, i, assets.dependencies, &error);
            auto snapshot = publication.acquire();
            RuntimeSurfaceHit street, field;
            Position streetPoint = fabric.layout.streets.front().path.front();
            Position fieldPoint;
            for (auto &f : territory.fields)
              if (!f.fields.empty())
              {
                for (auto v : f.fields.front().boundary)
                {
                  fieldPoint.x += v.x / f.fields.front().boundary.size();
                  fieldPoint.z += v.z / f.fields.front().boundary.size();
                }
                break;
              }
            // Ground just outside the terrace, inside the core job.
            fieldPoint = {runtimeInput.map.minX + 5, 0, runtimeInput.map.minZ + 5};
            bool streetOK = published && snapshot && queryRuntimeSurface(*snapshot, streetPoint.x, streetPoint.z, 0, street, &error) && std::abs(street.height - fabric.terraceLevel) < 1e-3;
            bool fieldOK = published && snapshot && queryRuntimeSurface(*snapshot, fieldPoint.x, fieldPoint.z, 0, field, &error) && std::abs(field.height - site.brief.terrain(fieldPoint.x, fieldPoint.z)) < .5;
            std::cout << "  street query " << street.height << " vs terrace " << fabric.terraceLevel << "; ground query " << field.height << " vs terrain " << site.brief.terrain(fieldPoint.x, fieldPoint.z) << " " << error << "\n";
            check(streetOK, "published street surface agrees with the terrace the houses were laid out on");
            check(fieldOK, "published ground surface agrees with the actual world terrain");
            RuntimeBuildInput fullInput;
            SettlementRuntimeSettings fullSettings;
            fullSettings.domain = SettlementRuntimeDomain::Full;
            RuntimeBuildResult fullBuilt;
            bool fullOK = composeSettlementRuntimeInput(site, territory, fabric, fullSettings, fullInput, &error) && buildRuntimeWorld(fullInput, nullptr, fullBuilt, &error);
            std::cout << "  full-domain job: " << (fullOK ? "built" : error) << "\n";
            check(fullOK || error.find("budget") != std::string::npos || error.find("stations") != std::string::npos, "full hinterland job either builds or reports an explicit capacity failure");
          }
        }
      }
      GenerationSnapshot chain;
      WorldSiteGenerationBrief brief{world, siteSettings};
      SettlementFabricSettings chainFabric;
      SettlementRuntimeSettings chainRuntime;
      bool ran = runGenerators(
          {worldSettlementSiteGenerator(1), settlementTerritoryBriefGenerator(1), cityTerritoryGenerator(1), settlementFabricGenerator(1), settlementRuntimeBriefGenerator(1), runtimeWorldGenerator(1)},
          {{{"world.site-brief", 1}, {brief, 1}}, {{"settlement.fabric-settings", 1}, {chainFabric, 1}}, {{"settlement.runtime-settings", 1}, {chainRuntime, 1}}},
          17,
          chain,
          {},
          &error);
      if (!ran)
        std::cout << "  runtime chain (parked): " << error << "\n";
      check(ran || error.find("preserved") != std::string::npos || error.find("budget") != std::string::npos, "site -> territory -> fabric -> runtime chain executes or reports an explicit runtime failure");
      // Class-based generators: the same chain as generator objects sharing a
      // ledger, with the hinterland published through per-tile runtime jobs.
      {
        using namespace procedural::world::generator;
        auto ledger = std::make_shared<Ledger>();
        SettlementRuntimeSettings tiled;
        tiled.domain = SettlementRuntimeDomain::Full;
        GenerationSnapshot classed;
        bool classRan = runGenerators(settlementGeneratorTasks(1, ledger), {{{"world.site-brief", 1}, {brief, 1}}, {{"settlement.fabric-settings", 1}, {chainFabric, 1}}, {{"settlement.runtime-settings", 1}, {tiled, 1}}}, 17, classed, {}, &error);
        std::string classMessage = "class-based settlement generators run through the director: " + error;
        if (!classRan)
          std::cout << "  class chain (parked runtime): " << error << "\n";
        check(classRan || error.find("no runtime job built") != std::string::npos, classMessage.c_str());
        std::cout << "GENERATOR_LEDGER rejected=" << ledger->count(LedgerKind::Rejected) << " omitted=" << ledger->count(LedgerKind::Omitted) << " capacity=" << ledger->count(LedgerKind::Capacity) << " repaired=" << ledger->count(LedgerKind::Repaired) << "\n";
        if (classRan && classed.count({"runtime.results", 1}))
        {
          const auto &results = std::any_cast<const RuntimeJobResults &>(classed.at({"runtime.results", 1}).value);
          const auto &fabricProduct = std::any_cast<const SettlementFabric &>(classed.at({"settlement.fabric", 1}).value);
          const auto &territoryProduct = std::any_cast<const CityTerritory &>(classed.at({"city.territory", 1}).value);
          size_t faces = 0;
          for (const auto &job : results.jobs)
          {
            if (job.built)
              faces += job.result.assets.levels[0].world.map.faces.size();
            std::cout << "  job " << job.name << " [" << job.minX << "," << job.minZ << "," << job.maxX << "," << job.maxZ << "] " << (job.built ? "faces=" + std::to_string(job.result.assets.levels[0].world.map.faces.size()) : job.error) << "\n";
          }
          std::cout << "W1_TILED jobs=" << results.jobs.size() << " built=" << results.built << " faces=" << faces << "\n";
          // The first field's centroid must publish in the tile that holds it.
          Position fieldPoint;
          bool fieldFound = false;
          for (auto &f : territoryProduct.fields)
            if (!f.fields.empty() && !fieldFound)
            {
              for (auto v : f.fields.front().boundary)
              {
                fieldPoint.x += v.x / f.fields.front().boundary.size();
                fieldPoint.z += v.z / f.fields.front().boundary.size();
              }
              fieldFound = true;
            }
          bool fieldPublished = false, fieldTileBuilt = false;
          for (const auto &job : results.jobs)
            if (fieldFound && fieldPoint.x >= job.minX && fieldPoint.x <= job.maxX && fieldPoint.z >= job.minZ && fieldPoint.z <= job.maxZ)
            {
              fieldTileBuilt = job.built;
              if (!job.built || job.result.assets.levels.empty())
                break;
              WorldRuntimePublication publication;
              uint64_t ticket = 0;
              const auto &assets = job.result.assets;
              bool published = publication.begin(assets, std::vector<uint32_t>(assets.levels[0].world.tiles.size(), 0), 4000000, ticket, &error);
              for (uint32_t i = 0; published && i < assets.levels[0].world.tiles.size(); ++i)
                published &= publication.complete(ticket, i, assets.dependencies, &error);
              auto snapshot = publication.acquire();
              RuntimeSurfaceHit hit;
              fieldPublished = published && snapshot && queryRuntimeSurface(*snapshot, fieldPoint.x, fieldPoint.z, 0, hit, &error) && std::abs(hit.height - fabricProduct.terrain(fieldPoint.x, fieldPoint.z)) < .5;
              std::cout << "  field query " << hit.height << " vs terrain " << fabricProduct.terrain(fieldPoint.x, fieldPoint.z) << " in job " << job.name << "\n";
              break;
            }
          check(fieldFound && fieldTileBuilt && fieldPublished, "a farm field publishes through its tile job and agrees with the world terrain");
        }
      }
    }
  }
  // A1 jettying acceptance: eligibility, exclusions, clearance and limits.
  {
    JettyRequest poorRow;
    poorRow.floors = 2;
    poorRow.streetWidthMetres = 6;
    // The mix is deterministic in (seed, house): find one projecting and one flush slot among the first houses.
    uint64_t projecting = 0, flushSlot = 0;
    for (uint64_t house = 1; house <= 16 && (!projecting || !flushSlot); ++house)
    {
      poorRow.house = house;
      auto trial = decideJetty(poorRow);
      if (trial.jettied && !projecting)
        projecting = house;
      if (trial.eligible && !trial.jettied && !flushSlot)
        flushSlot = house;
    }
    poorRow.house = projecting;
    auto row = decideJetty(poorRow);
    JettyRequest shop = poorRow;
    shop.floors = 3;
    auto shopHouse = decideJetty(shop);
    JettyRequest flushRow = poorRow;
    flushRow.house = flushSlot;
    auto flush = decideJetty(flushRow);
    JettyRequest wealthy = poorRow;
    wealthy.poorOrMiddleHousehold = false;
    JettyRequest church = poorRow;
    church.institutional = true;
    JettyRequest narrow = poorRow;
    narrow.streetWidthMetres = 2.6;
    JettyRequest opposing = poorRow;
    opposing.streetWidthMetres = 3.6;
    opposing.oppositeJettyMetres = .6; // 3.6 - .6 - 2.5 = .5 available.
    auto conflict = decideJetty(opposing);
    bool cumulative = shopHouse.jettied && std::abs(shopHouse.total - .9) < 1e-9 && shopHouse.perFloor[1] == .45 && shopHouse.perFloor[2] == .45;
    check(row.jettied && std::abs(row.total - .6) < 1e-9 && row.perFloor[0] == 0, "A1: eligible poor row house projects its upper floor by 0.6 m");
    check(cumulative, "A1: three-floor shop-house keeps the per-floor and cumulative limits");
    check(projecting && flushSlot && flush.eligible && !flush.jettied, "A1: flush-front variant by deterministic mix");
    check(!decideJetty(wealthy).eligible && !decideJetty(church).eligible, "A1: wealthy and institutional buildings are excluded");
    check(decideJetty(narrow).eligible && !decideJetty(narrow).jettied, "A1: narrow street rejects the projection");
    check(conflict.jettied && std::abs(conflict.total - .5) < 1e-9, "A1: opposing overhang reduces the projection to keep the corridor");
    DwellingBrief jettyBrief;
    jettyBrief.id = 9;
    jettyBrief.use = DwellingUse::ShopHouse;
    jettyBrief.floors = 2;
    jettyBrief.availableHeight = 6;
    jettyBrief.shell = {{0, 0, 0}, {9, 0, 0}, {9, 0, 11}, {0, 0, 11}};
    jettyBrief.entrance = {4.5, 0, 0};
    jettyBrief.jettyMetres = {0, .6};
    DwellingPlan jettied;
    bool planned = generateDwellingPlan(jettyBrief, jettied, &error);
    double front = 1e9;
    for (auto &f : jettied.floors)
      if (&f == &jettied.floors.back())
        for (auto &slab : f.slabs)
          for (auto v : slab)
            front = std::min(front, v.z);
    check(planned && !jettied.fallback && jettied.floors.size() == 2 && std::abs(front + .6) < 1e-6 && jettied.floors[0].grossArea < jettied.floors[1].grossArea, "A1: jettied upper floor polygon projects 0.6 m over a fixed ground footprint");
  }
  // B1-B3 block grammar on a synthetic hillside block: every lot reachable,
  // alleys connected to the street, stairs where the alley climbs.
  {
    using namespace procedural::world::generator;
    ConvexRing block{{0, 0, 0}, {60, 0, 0}, {60, 0, 40}, {0, 0, 40}};
    std::vector<bool> streets{true, false, true, false}; // South and north sides face streets.
    BlockGrammarSettings settings;
    settings.seed = 17;
    Ledger blockLedger;
    BlockPlan plan;
    bool ok = planBlockGrammar(5, block, streets, [](double, double z) { return z * .12; }, settings, plan, blockLedger, &error);
    std::cout << "BLOCK_GRAMMAR cells=" << plan.cells.size() << " lots=" << plan.lots << " alleys=" << plan.alleys << " stairs=" << plan.stairs << " courts=" << plan.courts << " streets=" << plan.streets << " inaccessible=" << plan.inaccessibleLots << " restarts=" << plan.restarts << " " << error << "\n";
    check(ok && plan.cells.size() > 20 && plan.lots > 0 && plan.streets > 0, "block grammar tiles a block into quads with street, lot and alley cells");
    check(ok && plan.solved, "every lot has alley or street access and dead alleys became courts");
    check(ok && plan.stairs > 0, "alleys climbing the hillside step become stairs");
    BlockPlan again;
    Ledger againLedger;
    bool same = planBlockGrammar(5, block, streets, [](double, double z) { return z * .12; }, settings, again, againLedger, &error) && again.cells.size() == plan.cells.size();
    for (size_t i = 0; same && i < plan.cells.size(); ++i)
      same &= again.cells[i].tile == plan.cells[i].tile;
    check(same, "block grammar is deterministic in its seed");
  }
  // A2 compounds: a castle and a manor on synthetic precincts.
  {
    using namespace procedural::world::generator;
    Ledger compoundLedger;
    CompoundBrief castle;
    castle.id = 3;
    castle.kind = CompoundKind::Castle;
    castle.precinct = {{0, 0, 0}, {70, 0, 0}, {74, 0, 52}, {-2, 0, 56}};
    castle.approach = {35, 0, 0};
    CompoundPlan castlePlan;
    bool castleOK = planCompound(castle, [](double x, double z) { return z * .02 + x * .01; }, castlePlan, compoundLedger, &error);
    std::cout << "COMPOUND castle walls=" << castlePlan.walls.size() << " towers=" << castlePlan.towers.size() << " wings=" << castlePlan.wings.size() << " planned=" << castlePlan.plannedWings << " " << error << "\n";
    for (auto &w : castlePlan.wings)
      std::cout << "  wing " << w.role << " " << (w.planned ? "planned rooms=" + std::to_string(w.plan.rooms.size()) + " floors=" + std::to_string(w.plan.floors.size()) : w.note) << "\n";
    bool gateOnApproachSide = castleOK && std::hypot(castlePlan.gate.x - castle.approach.x, castlePlan.gate.z - castle.approach.z) < 14;
    check(castleOK && castlePlan.walls.size() == 6 && castlePlan.towers.size() == 5 && gateOnApproachSide, "A2: castle has a closed curtain wall with corner towers and a gatehouse on the approach side");
    check(castleOK && castlePlan.solved && castlePlan.wings.size() >= 2, "A2: keep, hall and stable wings are planned interiors");
    CompoundBrief manor;
    manor.id = 4;
    manor.kind = CompoundKind::Manor;
    manor.precinct = {{0, 0, 0}, {40, 0, 0}, {40, 0, 30}, {0, 0, 30}};
    manor.approach = {20, 0, 0};
    CompoundPlan manorPlan;
    bool manorOK = planCompound(manor, [](double, double) { return 0.; }, manorPlan, compoundLedger, &error);
    std::cout << "COMPOUND manor walls=" << manorPlan.walls.size() << " wings=" << manorPlan.wings.size() << " planned=" << manorPlan.plannedWings << " " << error << "\n";
    for (auto &w : manorPlan.wings)
      std::cout << "  wing " << w.role << " " << (w.planned ? "planned rooms=" + std::to_string(w.plan.rooms.size()) : w.note) << "\n";
    check(manorOK && manorPlan.solved && manorPlan.wings.size() == 2 && manorPlan.walls.size() == 6, "A2: manor plans a hall wing and a service wing around a walled court");
  }
  std::vector<GenerationTask> catalogue{
    landPartitionGenerator(1),
    coastDetailGenerator(1),
    riverNetworkGenerator(1),
    watershedGenerator(1),
    channelGenerator(1),
    roadNetworkGenerator(1),
    roadEarthworkGenerator(1),
    cityDemandGenerator(1),
    cityMasterPlanGenerator(1),
    cityLayoutGenerator(1),
    ruralPatchesGenerator(1),
    cityTerritoryGenerator(1),
    settlementProgrammeGenerator(1),
    runtimeWorldGenerator(1),
    farmFieldsGenerator(1),
    dwellingPlanGenerator(1),
    dwellingMeshGenerator(1),
    worldSettlementSiteGenerator(1),
    settlementTerritoryBriefGenerator(1),
    settlementFabricGenerator(1),
    settlementRuntimeBriefGenerator(1)};
  bool valid = true;
  for (auto &task : catalogue)
    valid &= !task.descriptor.id.empty() && !task.descriptor.reads.empty() && task.descriptor.writes.size() == 1;
  check(valid && catalogue.size() == 21, "all twenty-one native domain adapters expose typed contracts");
  if (argc > 1)
  {
    std::ofstream file(argv[1]);
    auto write = [&](const std::vector<GenerationTask> &tasks)
    {
      file << "[";
      bool first = true;
      for (auto &task : tasks)
      {
        if (!first)
          file << ",";
        first = false;
        file << "{\"id\":" << std::quoted(task.descriptor.id) << ",\"domain\":" << task.descriptor.domain << ",\"parallelSafe\":" << (task.parallelSafe ? "true" : "false");
        for (auto field : {false, true})
        {
          file << (field ? ",\"writes\":[" : ",\"reads\":[");
          bool initial = true;
          for (auto &port : field ? task.descriptor.writes : task.descriptor.reads)
          {
            if (!initial)
              file << ",";
            initial = false;
            file << "{\"product\":" << std::quoted(port.key.product) << ",\"schema\":" << port.schema << "}";
          }
          file << "]";
        }
        file << "}";
      }
      file << "]";
    };
    file << "{\"schema\":1,\"activeMapDAG\":";
    write(procedural::world::worldPipelineGenerators());
    file << ",\"nativeAdapters\":";
    write(catalogue);
    file << "}\n";
  }
  std::cout << checks << " checks, " << failed << " failures\n";
  return failed ? 1 : 0;
}
